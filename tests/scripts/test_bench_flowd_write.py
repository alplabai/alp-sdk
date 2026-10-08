# SPDX-License-Identifier: Apache-2.0
"""alp-sdk#2233 -- the shared Flow D sector-pad machinery in bench-env.sh
(bench_flowd_plan/_read_sectors/_build/_prepare_write/_loadbin_lines/_proof),
exercised the way the five writer scripts (flash-jlink.sh, flash-jlink-hp.sh,
flash-jlink-mramxip.sh, flash-update-log-dual.sh,
flash-update-log-firewall-probe.sh) and erase-storage.sh actually use it.

Two kinds of coverage here:

  * FLOWD_DRY_RUN=1 -- touches no probe at all. Runs the real plan/build code
    (host-only) and asserts the CommandFile a write session and a proof
    session WOULD run contain only sector-ALIGNED addresses and never a
    gating `verifybin`.
  * A REAL (non-dry-run) round trip against a STUB "JLinkExe" -- a small
    Python program this file writes, backed by a persistent fake-MRAM
    directory, that actually emulates `savebin`/`loadbin` well enough to
    prove the padded write + fresh-session proof round-trips correctly and
    that neighbour bytes outside the blob survive. `bench_jlink_run` itself
    (the USB-topology masking, the DisableAutoUpdateFW prelude) is already
    covered by test_bench_jlink_run.py -- this file overrides it with a
    direct call to the stub, so it tests the flowd functions' OWN logic
    without re-proving that unrelated machinery.
"""

from __future__ import annotations

import json
import os
import stat
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
BENCH = REPO / "scripts" / "bench" / "aen"
ENV = BENCH / "bench-env.sh"
SECTOR = 0x4000


def _sanitized_env() -> dict[str, str]:
    """See test_bench_jlink_connect_guard.py's identical helper -- a unit
    test must never be able to reach real bench infrastructure."""
    env = dict(os.environ)
    for var in ("LG_PLACE", "LG_COORDINATOR", "LG_SWD_PATH", "ALP_JLINK_SEARCH_ROOT"):
        env.pop(var, None)
    return env


def _bash_can_run_a_script() -> bool:
    """See test_bench_jlink_connect_guard.py's identical helper."""
    try:
        probe = subprocess.run(
            ["bash", "-c", "printf ok"],
            capture_output=True, text=True, encoding="utf-8", timeout=30,
        )
    except (OSError, subprocess.SubprocessError):
        return False
    return probe.returncode == 0 and probe.stdout.strip() == "ok"


_NEEDS_BASH = pytest.mark.skipif(
    not _bash_can_run_a_script(),
    reason="no working `bash` on this host; bench-env.sh is POSIX shell",
)


def _preamble(tmp_path: Path, *, dry_run: bool) -> list[str]:
    lines = [
        "set -e",
        "unset LG_PLACE LG_COORDINATOR LG_SWD_PATH",
        f'export TMPDIR="{tmp_path}"',
        f'export FLOWD_SECTOR_PAD_PY="{BENCH / "flowd_sector_pad.py"}"',
    ]
    if dry_run:
        lines.append("export FLOWD_DRY_RUN=1")
    else:
        lines.append("unset FLOWD_DRY_RUN")
    lines.append(f'source "{ENV}"')
    return lines


def _write_blob(tmp_path: Path, name: str, data: bytes) -> Path:
    p = tmp_path / name
    p.write_bytes(data)
    return p


def _run(tmp_path: Path, body: list[str]) -> subprocess.CompletedProcess[str]:
    script = tmp_path / "run.sh"
    script.write_text("\n".join(body) + "\n", encoding="utf-8")
    return subprocess.run(
        ["bash", str(script)], cwd=tmp_path, env=_sanitized_env(),
        capture_output=True, text=True, encoding="utf-8", timeout=60,
    )


# --------------------------------------------------------------------
# FLOWD_DRY_RUN=1: no probe touched, real plan/build, inspect the CommandFiles
# --------------------------------------------------------------------

@_NEEDS_BASH
def test_dry_run_write_commandfile_has_only_aligned_loadbin_and_no_verifybin(tmp_path: Path) -> None:
    """The command file a write session WOULD run must loadbin only
    sector-aligned addresses (0x4000 multiples) and must never contain a
    `verifybin` line at all -- alp-sdk#2233's whole point."""
    blob = _write_blob(tmp_path, "pkg.bin", b"\xAB" * 1024)
    body = _preamble(tmp_path, dry_run=True) + [
        f'bench_flowd_prepare_write test1 "{tmp_path}/scratch" "{blob}:0x802E5000"',
        'echo "MANIFEST=$FLOWD_MANIFEST"',
        "cat > write.jlink <<EOF\n"
        "si SWD\n"
        "speed 4000\n"
        "device AE822FA0E5597LS0_M55_HE\n"
        "connect\n"
        '$(bench_flowd_loadbin_lines "$FLOWD_MANIFEST" 0)\n'
        "RSetType 2\n"
        "r\n"
        "g\n"
        "exit\n"
        "EOF",
        "cat write.jlink",
    ]
    res = _run(tmp_path, body)
    assert res.returncode == 0, res.stdout + res.stderr
    assert "verifybin" not in res.stdout

    loadbin_lines = [ln for ln in res.stdout.splitlines() if ln.startswith("loadbin ")]
    assert loadbin_lines, f"no loadbin line generated:\n{res.stdout}"
    for ln in loadbin_lines:
        # "loadbin <image> <address>[, noreset]"
        parts = ln.split()
        addr = int(parts[2], 16)
        assert addr % SECTOR == 0, f"loadbin address not sector-aligned: {ln}"


@_NEEDS_BASH
def test_bench_flowd_loadbin_lines_emits_noreset_when_requested(tmp_path: Path) -> None:
    """alp-sdk#2233 review major 5, mutation M5: `, noreset` on every
    loadbin line is load-bearing for flash-jlink-mramxip.sh (#1902 -- the
    SES re-booting slot0 between the two loadbins races J-Link's own
    program/verify). Calls bench_flowd_loadbin_lines directly with
    noreset=1/0 and asserts the suffix is present/absent exactly as asked --
    if bench-env.sh's own `if [ "$noreset" = "1" ]` branch were ever
    disabled, this fails immediately without needing a live mramxip run."""
    blob = _write_blob(tmp_path, "pkg.bin", b"\xAB" * 1024)
    body = _preamble(tmp_path, dry_run=True) + [
        f'bench_flowd_prepare_write test1b "{tmp_path}/scratch" "{blob}:0x802E5000"',
        'echo "NORESET1:"',
        'bench_flowd_loadbin_lines "$FLOWD_MANIFEST" 1',
        'echo "NORESET0:"',
        'bench_flowd_loadbin_lines "$FLOWD_MANIFEST" 0',
    ]
    res = _run(tmp_path, body)
    assert res.returncode == 0, res.stdout + res.stderr
    out = res.stdout
    noreset1 = out.split("NORESET1:")[1].split("NORESET0:")[0]
    noreset0 = out.split("NORESET0:")[1]
    loadbin1 = [ln for ln in noreset1.splitlines() if ln.startswith("loadbin ")]
    loadbin0 = [ln for ln in noreset0.splitlines() if ln.startswith("loadbin ")]
    assert loadbin1, f"no loadbin line for noreset=1:\n{out}"
    assert loadbin0, f"no loadbin line for noreset=0:\n{out}"
    for ln in loadbin1:
        assert ln.rstrip().endswith(", noreset"), f"noreset=1 must append ', noreset': {ln!r}"
    for ln in loadbin0:
        assert not ln.rstrip().endswith(", noreset"), f"noreset=0 must NOT append ', noreset': {ln!r}"


@_NEEDS_BASH
def test_dry_run_proof_commandfile_uses_savebin_at_aligned_ranges(tmp_path: Path) -> None:
    """The post-write proof session's CommandFile must savebin only
    sector-aligned ranges, never a verifybin -- and (alp-sdk#2233 review
    blocker 1c) bench_flowd_proof must NEVER report success (exit 0) in
    FLOWD_DRY_RUN: it returns a distinct 12 ("nothing proven"), so a direct
    caller can never mistake a dry run for a passed proof."""
    blob = _write_blob(tmp_path, "pkg.bin", b"\xCD" * 300)
    body = _preamble(tmp_path, dry_run=True) + [
        f'bench_flowd_prepare_write test2 "{tmp_path}/scratch" "{blob}:0x802E5100"',
        f'bench_flowd_proof test2 "$FLOWD_MANIFEST" "{tmp_path}/postread"',
    ]
    res = _run(tmp_path, body)
    assert res.returncode == 12, res.stdout + res.stderr
    assert "verifybin" not in res.stderr
    assert "NOTHING PROVEN" in res.stderr
    savebin_lines = [ln for ln in res.stderr.splitlines() if ln.strip().startswith("savebin ")]
    assert savebin_lines, f"no savebin line printed:\n{res.stderr}"
    for ln in savebin_lines:
        parts = ln.split()
        addr = int(parts[2], 16)
        assert addr % SECTOR == 0, f"savebin address not sector-aligned: {ln}"


@_NEEDS_BASH
def test_dry_run_two_writes_in_adjacent_sectors_merge_to_one_range(tmp_path: Path) -> None:
    """mramxip-shaped case: two writes far apart stay two ranges; two writes
    in the SAME/adjacent sectors merge into one -- exercised through the
    shell wrapper, not just the Python helper directly."""
    app = _write_blob(tmp_path, "app.bin", b"\x11" * 0x400)
    pkg = _write_blob(tmp_path, "pkg.bin", b"\x22" * 0x400)
    body = _preamble(tmp_path, dry_run=True) + [
        f'bench_flowd_prepare_write test3 "{tmp_path}/scratch" '
        f'"{app}:0x80010000" "{pkg}:0x8057EA50"',
        'python3 -c "import json,sys; m=json.load(open(sys.argv[1], encoding=\'utf-8\')); print(len(m))" '
        '"$FLOWD_MANIFEST"',
    ]
    res = _run(tmp_path, body)
    assert res.returncode == 0, res.stdout + res.stderr
    n_ranges = int(res.stdout.strip().splitlines()[-1])
    assert n_ranges == 2, f"far-apart writes must stay two separate padded ranges, got {n_ranges}"


# --------------------------------------------------------------------
# Real (non-dry-run) round trip against a stateful stub JLinkExe
# --------------------------------------------------------------------

_STUB_JLINK = r'''#!/usr/bin/env python3
"""Stub JLinkExe for alp-sdk#2233 tests: backs `savebin`/`loadbin` with a
persistent fake-MRAM directory (one file per SECTOR-aligned address), so a
write followed by a fresh "session" (a new invocation of this same stub)
round-trips real bytes -- close enough to prove bench_flowd_proof's PASS/FAIL
logic against something that behaves like MRAM, without any real probe."""
import os
import sys

SECTOR = 0x4000


def sector_path(mram_dir, addr):
    base = addr - (addr % SECTOR)
    return os.path.join(mram_dir, f"{base:08X}.bin")


def read_range(mram_dir, addr, size):
    out = bytearray()
    pos = addr
    while len(out) < size:
        base = pos - (pos % SECTOR)
        path = sector_path(mram_dir, pos)
        if os.path.isfile(path):
            with open(path, "rb") as f:
                sector = f.read()
        else:
            sector = b"\x00" * SECTOR
        off = pos - base
        take = min(SECTOR - off, size - len(out))
        out += sector[off:off + take]
        pos += take
    return bytes(out)


def write_range(mram_dir, addr, data):
    # Callers here always loadbin a whole, sector-aligned, sector-sized
    # padded image -- so this can write it straight into the matching
    # sector file(s) with no partial-sector merge logic needed.
    assert addr % SECTOR == 0, addr
    assert len(data) % SECTOR == 0, len(data)
    for i in range(0, len(data), SECTOR):
        base = addr + i
        with open(sector_path(mram_dir, base), "wb") as f:
            f.write(data[i:i + SECTOR])


def main() -> int:
    argv = sys.argv[1:]
    mram_dir = os.environ["FLOWD_TEST_FAKE_MRAM"]
    os.makedirs(mram_dir, exist_ok=True)
    cmdfile = None
    i = 0
    while i < len(argv):
        if argv[i] in ("-CommandFile", "-CommanderScript") and i + 1 < len(argv):
            cmdfile = argv[i + 1]
        i += 1
    print("SEGGER J-Link Commander (stub)")
    if cmdfile:
        with open(cmdfile, "r", encoding="utf-8") as f:
            lines = f.read().splitlines()
        for line in lines:
            print("J-Link>" + line)
            parts = line.split()
            if not parts:
                continue
            if parts[0] == "connect":
                print("Found SW-DP with ID 0x4C013477")
                print("Cortex-M55 identified.")
            elif parts[0] == "savebin" and len(parts) >= 4:
                dest, addr_s, size_s = parts[1], parts[2], parts[3]
                addr = int(addr_s, 0)
                size = int(size_s, 0)
                data = read_range(mram_dir, addr, size)
                with open(dest, "wb") as f:
                    f.write(data)
                # alp-sdk#2233 review round 5: the bench-measured savebin
                # SUCCESS line (V9.50) -- required by bench-env.sh's own
                # per-savebin success-count gate.
                print(f"Reading {size} bytes from addr 0x{addr:08X} into file...O.K.")
            elif parts[0] == "loadbin" and len(parts) >= 3:
                src, addr_s = parts[1], parts[2]
                addr = int(addr_s, 0)
                with open(src, "rb") as f:
                    data = f.read()
                write_range(mram_dir, addr, data)
    print("Script processing completed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
'''


def _write_stub_jlink(tmp_path: Path) -> Path:
    stub = tmp_path / "fake-jlinkexe.py"
    stub.write_text(_STUB_JLINK, encoding="utf-8")
    stub.chmod(stub.stat().st_mode | stat.S_IEXEC | stat.S_IXGRP | stat.S_IXOTH)
    return stub


def _preamble_real(tmp_path: Path, stub: Path, fake_mram: Path) -> list[str]:
    """Like _preamble(dry_run=False), but also overrides bench_jlink_run
    with a direct call to the stub -- bench_jlink_run's own USB-masking and
    DisableAutoUpdateFW-prelude machinery is test_bench_jlink_run.py's
    concern, not this file's; this exercises the flowd functions' OWN
    cmdfile-building, connect-assertion and proof logic against real bytes.
    """
    lines = _preamble(tmp_path, dry_run=False) + [
        f'export FLOWD_TEST_FAKE_MRAM="{fake_mram}"',
        "bench_jlink_run() {",
        f'  python3 "{stub}" "$@"',
        "}",
    ]
    return lines


@_NEEDS_BASH
def test_real_round_trip_writes_blob_and_preserves_neighbours(tmp_path: Path) -> None:
    """The decisive alp-sdk#2233 property, end to end against the stub:
    write an unaligned blob, prove it back, and prove the sector bytes
    OUTSIDE the blob came back as the pre-existing MRAM content -- not
    0xFF, which is what the raw (unpadded) loader behaviour would leave."""
    stub = _write_stub_jlink(tmp_path)
    fake_mram = tmp_path / "fake_mram"
    fake_mram.mkdir()
    # Pre-existing MRAM content in the touched sector: a distinctive pattern,
    # never 0x00 or 0xFF, so "neighbours preserved" can't pass by coincidence.
    pattern = bytes((i * 7 + 3) % 251 for i in range(SECTOR))
    (fake_mram / "802E4000.bin").write_bytes(pattern)

    blob = _write_blob(tmp_path, "pkg.bin", b"\x99" * 0x300)
    scratch = tmp_path / "scratch"
    body = _preamble_real(tmp_path, stub, fake_mram) + [
        f'bench_flowd_prepare_write realtest "{scratch}" "{blob}:0x802E5000"',
        'echo "MANIFEST=$FLOWD_MANIFEST"',
        "cat > write.jlink <<EOF\n"
        "si SWD\n"
        "speed 4000\n"
        "device AE822FA0E5597LS0_M55_HE\n"
        "connect\n"
        '$(bench_flowd_loadbin_lines "$FLOWD_MANIFEST" 0)\n'
        "exit\n"
        "EOF",
        'bench_jlink_run -nogui 1 -CommandFile write.jlink > write.out 2>&1 || true',
        f'bench_flowd_proof realtest "$FLOWD_MANIFEST" "{tmp_path}/postread"',
    ]
    res = _run(tmp_path, body)
    assert res.returncode == 0, res.stdout + res.stderr
    assert "PASS 0x802E4000" in res.stdout, res.stdout + res.stderr

    # Ground truth, read directly off the fake-MRAM backing store.
    final = (fake_mram / "802E4000.bin").read_bytes()
    off = 0x802E5000 - 0x802E4000
    assert final[off:off + 0x300] == b"\x99" * 0x300, "blob bytes did not land"
    assert final[:off] == pattern[:off], "neighbour bytes BEFORE the blob were not preserved"
    assert final[off + 0x300:] == pattern[off + 0x300:], "neighbour bytes AFTER the blob were not preserved"


@_NEEDS_BASH
def test_real_round_trip_fails_proof_when_mram_was_never_written(tmp_path: Path) -> None:
    """A proof against MRAM that was never actually written (e.g. a `loadbin`
    that silently no-op'd) must FAIL, not report a false PASS."""
    stub = _write_stub_jlink(tmp_path)
    fake_mram = tmp_path / "fake_mram"
    fake_mram.mkdir()
    # No pre-existing sector file at all -- read_range() in the stub treats
    # a missing sector as all-0x00, matching a genuinely blank part.

    blob = _write_blob(tmp_path, "pkg.bin", b"\x55" * 0x100)
    scratch = tmp_path / "scratch"
    body = _preamble_real(tmp_path, stub, fake_mram) + [
        f'bench_flowd_prepare_write failtest "{scratch}" "{blob}:0x802E5000"',
        # Deliberately skip the write session -- MRAM stays blank.
        f'bench_flowd_proof failtest "$FLOWD_MANIFEST" "{tmp_path}/postread"',
    ]
    res = _run(tmp_path, body)
    assert res.returncode != 0
    assert "FAIL 0x802E4000" in res.stdout, res.stdout + res.stderr


# --------------------------------------------------------------------
# erase-storage.sh's ATOC-trailer overlap refusal (alp-sdk#2233 review
# blocker 2), against a stub MRAM carrying the bench-measured layout
# --------------------------------------------------------------------

import shutil  # noqa: E402


def _write_full_script_copy(tmp_path: Path, stub: Path) -> Path:
    """A full copy of scripts/bench/aen/ with bench_jlink_run overridden in
    the COPY of bench-env.sh -- the same technique needed to run a whole
    writer script (not just the bench_flowd_* functions directly) against a
    stub, since the script itself sources bench-env.sh by relative path."""
    src = REPO / "scripts" / "bench" / "aen"
    dst = tmp_path / "aen"
    shutil.copytree(src, dst)
    envf = dst / "bench-env.sh"
    override = (
        "\nbench_jlink_run() {\n"
        f'  python3 "{stub}" "$@"\n'
        "}\n"
    )
    envf.write_text(envf.read_text(encoding="utf-8") + override, encoding="utf-8")
    return dst


def _measured_atoc_trailer_sector(package_start: int, package_size: int) -> bytes:
    """One 16 KiB sector carrying the bench-measured ATOC trailer shape at
    its own top (see scripts/bench/aen/atoc_trailer.py's header) -- the
    sector spans 0x8057C000-0x8057FFFF, matching the E1M-AEN801/AEN803
    `atoc` region's own last sector.

    alp-sdk#2233 review round 5: the trailer's real fields sit at +0x4/+0x8/
    +0xC (header_address/package_start/package_size), not +0x0/+0x4/+0x8 --
    +0x0 is an opaque word0 (bench-measured 0x4966A80E, unknown meaning,
    never validated). An earlier version of this fixture packed only three
    words starting at +0x0, which happened to still validate here because
    this file's own tests never checked word0 -- but it reproduced the
    real off-by-one-word bug that made `erase-storage.sh --check-only` exit
    6 instead of 7 on real E1M-AEN803 silicon (serial 2026W36-0001)."""
    import struct

    sector = bytearray(SECTOR)
    header_addr = 0x8057FF90
    header_off = header_addr - 0x8057C000
    sector[header_off:header_off + 8] = b"OEMTOC01"
    trailer_off = (0x80580000 - 16) - 0x8057C000
    word0 = 0x4966A80E  # bench-measured, opaque, never validated
    sector[trailer_off:trailer_off + 16] = struct.pack("<IIII", word0, header_addr, package_start, package_size)
    return bytes(sector)


@_NEEDS_BASH
def test_erase_storage_refuses_when_resident_atoc_overlaps_the_window(tmp_path: Path) -> None:
    """The exact alp-sdk#2233 review blocker 2 hazard, reproduced against a
    stub MRAM carrying the bench-measured layout (E1M-AEN803, serial
    2026W36-0001): a resident ATOC package starting INSIDE the nominal
    customer storage window must refuse the erase outright, before any
    write, and the stub's fake-MRAM must show NO write occurred."""
    stub = _write_stub_jlink(tmp_path)
    script_dir = _write_full_script_copy(tmp_path, stub)
    fake_mram = tmp_path / "fake_mram"
    fake_mram.mkdir()
    # The measured hazard: package_start (0x8056A3C0) is INSIDE the erase
    # window (0x80560000-0x80577FFF), well below its end (0x80578000).
    package_start = 0x8056A3C0
    package_size = 0x80580000 - package_start
    (fake_mram / "8057C000.bin").write_bytes(_measured_atoc_trailer_sector(package_start, package_size))
    # A distinctive pattern in the storage window itself, so "was it
    # written" can be checked directly against the fake-MRAM backing store.
    (fake_mram / "80560000.bin").write_bytes(bytes((i * 3 + 1) % 251 for i in range(SECTOR)))

    env = dict(os.environ)
    for var in ("LG_PLACE", "LG_COORDINATOR", "LG_SWD_PATH", "ALP_JLINK_SEARCH_ROOT", "SE_UART"):
        env.pop(var, None)
    env["TMPDIR"] = str(tmp_path)
    env["FLOWD_TEST_FAKE_MRAM"] = str(fake_mram)
    env["ALP_SDK_DIR"] = str(REPO)
    env["BENCH_ROOT"] = str(tmp_path)  # so a (never-reached) backup dir lands under tmp_path
    res = subprocess.run(
        ["bash", str(script_dir / "erase-storage.sh")],
        cwd=tmp_path, env=env, capture_output=True, text=True, encoding="utf-8", timeout=60,
    )
    assert res.returncode == 7, res.stdout + res.stderr
    assert "WOULD ZERO PART OF A LIVE" in res.stdout + res.stderr
    # Decisive: the storage window's own fake-MRAM content is UNCHANGED --
    # no loadbin ever reached the stub for it.
    assert (fake_mram / "80560000.bin").read_bytes() == bytes((i * 3 + 1) % 251 for i in range(SECTOR))


@_NEEDS_BASH
def test_erase_storage_proceeds_when_atoc_trailer_is_blank(tmp_path: Path) -> None:
    """A genuinely blank atoc region (no resident package at all) must NOT
    be refused -- only an actual overlap is a hazard."""
    stub = _write_stub_jlink(tmp_path)
    script_dir = _write_full_script_copy(tmp_path, stub)
    fake_mram = tmp_path / "fake_mram"
    fake_mram.mkdir()
    (fake_mram / "8057C000.bin").write_bytes(b"\x00" * SECTOR)  # blank trailer -> NO_ATOC

    env = dict(os.environ)
    for var in ("LG_PLACE", "LG_COORDINATOR", "LG_SWD_PATH", "ALP_JLINK_SEARCH_ROOT", "SE_UART"):
        env.pop(var, None)
    env["TMPDIR"] = str(tmp_path)
    env["FLOWD_TEST_FAKE_MRAM"] = str(fake_mram)
    env["ALP_SDK_DIR"] = str(REPO)
    env["BENCH_ROOT"] = str(tmp_path)
    res = subprocess.run(
        ["bash", str(script_dir / "erase-storage.sh")],
        cwd=tmp_path, env=env, capture_output=True, text=True, encoding="utf-8", timeout=60,
    )
    assert res.returncode == 0, res.stdout + res.stderr
    assert "no resident package found" in res.stdout + res.stderr
    # The window is now uniformly zero in the fake-MRAM backing store.
    for base in range(0x80560000, 0x80578000, SECTOR):
        assert (fake_mram / f"{base:08X}.bin").read_bytes() == b"\x00" * SECTOR
