# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/bench/aen/flowd_sector_pad.py (alp-sdk#2233).

Pure host-side logic, no probe: covers the alignment math, the merge-in-one-
sector case, neighbour preservation, every refusal, and proof PASS/FAIL --
both as a library (import) and through the CLI (subprocess), since Flow D
writer scripts drive this as a subprocess and a CLI-arg regression (a flag
rename, a bad exit code) would not show up in library-only tests.
"""
from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "bench" / "aen" / "flowd_sector_pad.py"
SECTOR = 0x4000
WINDOW_LO = 0x80000000
WINDOW_HI = 0x8057FFFF


@pytest.fixture(scope="module")
def fsp():
    spec = importlib.util.spec_from_file_location("flowd_sector_pad", SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules["flowd_sector_pad"] = mod
    spec.loader.exec_module(mod)
    return mod


def _run_cli(args: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args],
        cwd=cwd, capture_output=True, text=True, encoding="utf-8", timeout=30,
    )


def _write_blob(path: Path, data: bytes) -> Path:
    path.write_bytes(data)
    return path


def _write_sector(dirpath: Path, base: int, fill: bytes = b"\x00") -> None:
    dirpath.mkdir(parents=True, exist_ok=True)
    (dirpath / f"{base:08X}.bin").write_bytes(fill * SECTOR if len(fill) == 1 else fill)


# --------------------------------------------------------------------
# Alignment math
# --------------------------------------------------------------------

def test_align_down_and_up(fsp) -> None:
    assert fsp.align_down(0x802E5000, SECTOR) == 0x802E4000
    assert fsp.align_up(0x802E5400, SECTOR) == 0x802E8000
    # Already aligned: align_down is a no-op, align_up stays put.
    assert fsp.align_down(0x802E4000, SECTOR) == 0x802E4000
    assert fsp.align_up(0x802E4000, SECTOR) == 0x802E4000


def test_unaligned_start_and_end_pads_to_one_sector(fsp) -> None:
    w = fsp.Write(blob_path="b", address=0x802E5000, data=b"\x11" * 0x400)
    ranges = fsp.merge_ranges([w], SECTOR)
    assert len(ranges) == 1
    assert ranges[0].start == 0x802E4000
    assert ranges[0].end == 0x802E8000
    assert ranges[0].size == SECTOR


def test_blob_crossing_a_sector_boundary_spans_two_sectors(fsp) -> None:
    # 0x802E7F00 .. 0x802E8100 crosses the 0x802E8000 boundary.
    w = fsp.Write(blob_path="b", address=0x802E7F00, data=b"\x22" * 0x200)
    ranges = fsp.merge_ranges([w], SECTOR)
    assert len(ranges) == 1
    assert ranges[0].start == 0x802E4000
    assert ranges[0].end == 0x802EC000
    assert fsp.sector_bases(ranges[0].start, ranges[0].end, SECTOR) == [0x802E4000, 0x802E8000]


def test_exact_sector_blob_is_a_no_op_pad(fsp) -> None:
    w = fsp.Write(blob_path="b", address=0x802E4000, data=b"\x33" * SECTOR)
    ranges = fsp.merge_ranges([w], SECTOR)
    assert len(ranges) == 1
    assert (ranges[0].start, ranges[0].end) == (0x802E4000, 0x802E8000)
    assert ranges[0].size == SECTOR


# --------------------------------------------------------------------
# Merging two writes in one sector + neighbour preservation
# --------------------------------------------------------------------

def test_two_writes_in_one_sector_merge_into_one_range(fsp) -> None:
    a = fsp.Write(blob_path="a", address=0x802E5000, data=b"\xAA" * 0x100)
    b = fsp.Write(blob_path="b", address=0x802E5200, data=b"\xBB" * 0x100)
    ranges = fsp.merge_ranges([a, b], SECTOR)
    assert len(ranges) == 1
    assert {w.blob_path for w in ranges[0].writes} == {"a", "b"}


def test_second_blob_sees_first_blobs_bytes_not_stale_padding(fsp, tmp_path: Path) -> None:
    """The spec's decisive case: two writes sharing a sector must both land
    in the SAME padded image, each seeing the other's bytes, not just the
    pre-read (stale) sector content."""
    a = _write_blob(tmp_path / "a.bin", b"\xAA" * 0x100)
    b = _write_blob(tmp_path / "b.bin", b"\xBB" * 0x100)
    sector_dir = tmp_path / "sectors"
    _write_sector(sector_dir, 0x802E4000, b"\x00")

    writes = fsp.load_writes([f"{a}:0x802E5000", f"{b}:0x802E5200"])
    ranges = fsp.merge_ranges(writes, SECTOR)
    image = fsp.build_padded_image(ranges[0], str(sector_dir), SECTOR)

    off_a = 0x802E5000 - 0x802E4000
    off_b = 0x802E5200 - 0x802E4000
    assert image[off_a:off_a + 0x100] == b"\xAA" * 0x100
    assert image[off_b:off_b + 0x100] == b"\xBB" * 0x100
    # Everything else in the sector is untouched padding (0x00 here) --
    # NOT 0xFF, which is what the raw loader would have left behind.
    assert image[0:off_a] == b"\x00" * off_a


def test_neighbours_outside_the_blob_are_preserved_from_the_pre_read(fsp, tmp_path: Path) -> None:
    blob = _write_blob(tmp_path / "blob.bin", b"\x99" * 0x400)
    sector_dir = tmp_path / "sectors"
    # A distinctive pre-read pattern -- proves it's the READ content that
    # survives, not a zero-fill or an 0xFF-fill coincidence.
    pattern = bytes((i % 251) for i in range(SECTOR))
    (sector_dir).mkdir()
    (sector_dir / "802E4000.bin").write_bytes(pattern)

    writes = fsp.load_writes([f"{blob}:0x802E5000"])
    ranges = fsp.merge_ranges(writes, SECTOR)
    image = fsp.build_padded_image(ranges[0], str(sector_dir), SECTOR)

    off = 0x802E5000 - 0x802E4000
    assert image[:off] == pattern[:off]
    assert image[off:off + 0x400] == b"\x99" * 0x400
    assert image[off + 0x400:] == pattern[off + 0x400:]


# --------------------------------------------------------------------
# Refusals
# --------------------------------------------------------------------

def test_refuses_overlap_with_different_bytes(fsp, tmp_path: Path) -> None:
    a = _write_blob(tmp_path / "a.bin", b"\xAA" * 0x200)
    b = _write_blob(tmp_path / "b.bin", b"\xBB" * 0x200)
    writes = fsp.load_writes([f"{a}:0x802E5000", f"{b}:0x802E5100"])
    with pytest.raises(fsp.FlowdSectorPadError, match="different bytes"):
        fsp.check_blob_conflicts(writes)


def test_allows_overlap_with_identical_bytes(fsp, tmp_path: Path) -> None:
    a = _write_blob(tmp_path / "a.bin", b"\xAA" * 0x200)
    b = _write_blob(tmp_path / "b.bin", b"\xAA" * 0x100)  # subset, same bytes
    writes = fsp.load_writes([f"{a}:0x802E5000", f"{b}:0x802E5080"])
    fsp.check_blob_conflicts(writes)  # must not raise


def test_refuses_range_outside_the_mram_window(fsp, tmp_path: Path) -> None:
    blob = _write_blob(tmp_path / "b.bin", b"\x11" * 0x10)
    writes = fsp.load_writes([f"{blob}:0x70000000"])
    with pytest.raises(fsp.FlowdSectorPadError, match="outside the MRAM window"):
        fsp.validate_window(writes, WINDOW_LO, WINDOW_HI)


def test_refuses_range_crossing_the_window_high_bound(fsp, tmp_path: Path) -> None:
    blob = _write_blob(tmp_path / "b.bin", b"\x11" * 0x10)
    writes = fsp.load_writes([f"{blob}:0x8057FFF8"])  # ends past WINDOW_HI
    with pytest.raises(fsp.FlowdSectorPadError, match="outside the MRAM window"):
        fsp.validate_window(writes, WINDOW_LO, WINDOW_HI)


def test_refuses_missing_pre_read_sector(fsp, tmp_path: Path) -> None:
    blob = _write_blob(tmp_path / "b.bin", b"\x11" * 0x10)
    sector_dir = tmp_path / "sectors"
    sector_dir.mkdir()
    writes = fsp.load_writes([f"{blob}:0x802E5000"])
    ranges = fsp.merge_ranges(writes, SECTOR)
    with pytest.raises(fsp.FlowdSectorPadError, match="missing pre-read sector image"):
        fsp.build_padded_image(ranges[0], str(sector_dir), SECTOR)


def test_refuses_wrong_size_pre_read_sector(fsp, tmp_path: Path) -> None:
    blob = _write_blob(tmp_path / "b.bin", b"\x11" * 0x10)
    sector_dir = tmp_path / "sectors"
    sector_dir.mkdir()
    (sector_dir / "802E4000.bin").write_bytes(b"\x00" * (SECTOR - 1))  # short by one byte
    writes = fsp.load_writes([f"{blob}:0x802E5000"])
    ranges = fsp.merge_ranges(writes, SECTOR)
    with pytest.raises(fsp.FlowdSectorPadError, match="expected 16384"):
        fsp.build_padded_image(ranges[0], str(sector_dir), SECTOR)


# --------------------------------------------------------------------
# Proof PASS / FAIL
# --------------------------------------------------------------------

def test_proof_pass_and_fail(fsp, tmp_path: Path) -> None:
    blob = _write_blob(tmp_path / "b.bin", b"\x77" * 0x400)
    sector_dir = tmp_path / "sectors"
    _write_sector(sector_dir, 0x802E4000, b"\x00")
    out_dir = tmp_path / "out"

    build_args = ["build", "--write", f"{blob}:0x802E5000",
                  "--sector-dir", str(sector_dir), "--out-dir", str(out_dir)]
    res = _run_cli(build_args, cwd=tmp_path)
    assert res.returncode == 0, res.stderr
    manifest_path = out_dir / "manifest.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    assert len(manifest) == 1
    image_bytes = Path(manifest[0]["image"]).read_bytes()

    # PASS: the read-back is byte-identical to the padded image.
    read_dir_pass = tmp_path / "read_pass"
    read_dir_pass.mkdir()
    (read_dir_pass / "802E4000.bin").write_bytes(image_bytes)
    res = _run_cli(["proof", "--manifest", str(manifest_path), "--read-dir", str(read_dir_pass)], cwd=tmp_path)
    assert res.returncode == 0, res.stdout + res.stderr
    assert "PASS 0x802E4000" in res.stdout

    # FAIL: one flipped byte in the read-back.
    read_dir_fail = tmp_path / "read_fail"
    read_dir_fail.mkdir()
    corrupted = bytearray(image_bytes)
    corrupted[0] ^= 0xFF
    (read_dir_fail / "802E4000.bin").write_bytes(bytes(corrupted))
    res = _run_cli(["proof", "--manifest", str(manifest_path), "--read-dir", str(read_dir_fail)], cwd=tmp_path)
    assert res.returncode == 1
    assert "FAIL 0x802E4000" in res.stdout


def test_proof_fail_on_missing_read_back(fsp, tmp_path: Path) -> None:
    blob = _write_blob(tmp_path / "b.bin", b"\x77" * 0x10)
    sector_dir = tmp_path / "sectors"
    _write_sector(sector_dir, 0x802E4000, b"\x00")
    out_dir = tmp_path / "out"
    res = _run_cli(
        ["build", "--write", f"{blob}:0x802E5000", "--sector-dir", str(sector_dir), "--out-dir", str(out_dir)],
        cwd=tmp_path,
    )
    assert res.returncode == 0, res.stderr

    empty_read_dir = tmp_path / "empty"
    empty_read_dir.mkdir()
    res = _run_cli(
        ["proof", "--manifest", str(out_dir / "manifest.json"), "--read-dir", str(empty_read_dir)],
        cwd=tmp_path,
    )
    assert res.returncode == 1
    assert "missing read-back image" in res.stdout


# --------------------------------------------------------------------
# CLI plan mode (used directly by bench-env.sh's bench_flowd_write())
# --------------------------------------------------------------------

def test_cli_plan_lists_sector_bases(tmp_path: Path) -> None:
    blob = _write_blob(tmp_path / "b.bin", b"\x11" * 0x400)
    res = _run_cli(["plan", "--write", f"{blob}:0x802E5000"], cwd=tmp_path)
    assert res.returncode == 0, res.stderr
    assert res.stdout.splitlines() == ["0x802E4000"]


def test_cli_plan_refuses_and_exits_nonzero_on_bad_window(tmp_path: Path) -> None:
    blob = _write_blob(tmp_path / "b.bin", b"\x11" * 0x10)
    res = _run_cli(["plan", "--write", f"{blob}:0x70000000"], cwd=tmp_path)
    assert res.returncode != 0
    assert "REFUSE" in res.stderr
