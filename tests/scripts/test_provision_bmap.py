"""provision.bmap + provision/board_bmap.py: write and verify only the mapped
blocks of the system image (no hardware; the board-side script runs locally
against a plain file standing in for the eMMC)."""

from __future__ import annotations

import gzip
import hashlib
import subprocess
import sys
from pathlib import Path

import pytest
from provision import bmap

BLOCK = 4096
BOARD_SCRIPT = Path(bmap.__file__).with_name("board_bmap.py")


def image(blocks: int = 16, tail: int = 0) -> bytes:
    """`blocks` whole blocks, each filled with its own index, plus `tail` bytes."""
    return b"".join(bytes([i + 1]) * BLOCK for i in range(blocks)) + b"\x7f" * tail


def holes(img: bytes, ranges: list[tuple[int, int]]) -> bytes:
    """`img` with every block outside `ranges` zeroed: the image a bmap listing
    exactly those ranges describes."""
    out = bytearray(len(img))
    for first, last in ranges:
        out[first * BLOCK:(last + 1) * BLOCK] = img[first * BLOCK:(last + 1) * BLOCK]
    return bytes(out)


def bmap_xml(img: bytes, ranges: list[tuple[int, int]], checksum_type: str = "sha256",
             blocks_count: int | None = None) -> str:
    rows = []
    for first, last in ranges:
        digest = hashlib.sha256(img[first * BLOCK:(last + 1) * BLOCK]).hexdigest()
        span = str(first) if first == last else f"{first}-{last}"
        rows.append(f'        <Range chksum="{digest}"> {span} </Range>')
    count = -(-len(img) // BLOCK) if blocks_count is None else blocks_count
    mapped = sum(last - first + 1 for first, last in ranges)
    body = (
        '<?xml version="1.0" ?>\n<bmap version="2.0">\n'
        f"    <ImageSize> {len(img)} </ImageSize>\n"
        f"    <BlockSize> {BLOCK} </BlockSize>\n"
        f"    <BlocksCount> {count} </BlocksCount>\n"
        f"    <MappedBlocksCount> {mapped} </MappedBlocksCount>\n"
        f"    <ChecksumType> {checksum_type} </ChecksumType>\n"
        f"    <BmapFileChecksum> {'0' * 64} </BmapFileChecksum>\n"
        "    <BlockMap>\n" + "\n".join(rows) + "\n    </BlockMap>\n</bmap>\n")
    return body.replace("0" * 64, hashlib.sha256(body.encode()).hexdigest(), 1)


def gz(tmp_path: Path, img: bytes) -> Path:
    p = tmp_path / "img.wic.gz"
    p.write_bytes(gzip.compress(img, mtime=0))
    return p


# --- parse -------------------------------------------------------------------------

def test_parse_returns_byte_spans_of_the_mapped_ranges():
    img = image(16)
    bm = bmap.parse(bmap_xml(img, [(0, 1), (5, 5), (9, 12)]))
    assert bm.image_size == len(img) and bm.block_size == BLOCK
    assert bm.spans == ((0, 2 * BLOCK), (5 * BLOCK, 6 * BLOCK), (9 * BLOCK, 13 * BLOCK))
    assert bm.mapped_bytes == 7 * BLOCK


def test_parse_clips_the_last_span_to_an_image_that_ends_mid_block():
    img = image(4, tail=100)
    bm = bmap.parse(bmap_xml(img, [(0, 0), (4, 4)]))
    assert bm.spans == ((0, BLOCK), (4 * BLOCK, 4 * BLOCK + 100))


def test_parse_rejects_a_bmap_whose_own_checksum_does_not_match():
    xml = bmap_xml(image(), [(0, 1)]).replace("<ImageSize> 65536", "<ImageSize> 65537")
    with pytest.raises(ValueError, match="BmapFileChecksum"):
        bmap.parse(xml)


def test_parse_rejects_overlapping_or_unsorted_ranges():
    with pytest.raises(ValueError, match="overlap"):
        bmap.parse(bmap_xml(image(), [(0, 4), (4, 6)]))
    with pytest.raises(ValueError, match="overlap"):
        bmap.parse(bmap_xml(image(), [(8, 9), (0, 1)]))


def test_parse_rejects_a_range_past_the_image():
    with pytest.raises(ValueError, match="past the image"):
        bmap.parse(bmap_xml(image(16), [(0, 16)]))


def test_parse_rejects_a_checksum_type_other_than_sha256():
    with pytest.raises(ValueError, match="sha256"):
        bmap.parse(bmap_xml(image(), [(0, 1)], checksum_type="md5"))


def test_parse_rejects_text_that_is_not_a_bmap():
    with pytest.raises(ValueError, match="bmap"):
        bmap.parse("<notbmap/>")
    with pytest.raises(ValueError, match="bmap"):
        bmap.parse("not xml at all")


# --- verify_image ------------------------------------------------------------------

def test_verify_image_returns_the_md5_of_the_mapped_bytes(tmp_path):
    img = holes(image(16), [(0, 1), (9, 12)])
    bm = bmap.parse(bmap_xml(img, [(0, 1), (9, 12)]))
    want = hashlib.md5(img[:2 * BLOCK] + img[9 * BLOCK:13 * BLOCK]).hexdigest()
    assert bmap.verify_image(bm, gz(tmp_path, img)) == want


def test_verify_image_rejects_an_image_whose_mapped_range_differs(tmp_path):
    img = holes(image(16), [(0, 1), (9, 12)])
    bm = bmap.parse(bmap_xml(img, [(0, 1), (9, 12)]))
    other = bytearray(img)
    other[10 * BLOCK] ^= 0xFF
    with pytest.raises(ValueError, match="blocks 9-12"):
        bmap.verify_image(bm, gz(tmp_path, bytes(other)))


def test_verify_image_rejects_an_image_of_another_size(tmp_path):
    bm = bmap.parse(bmap_xml(image(16), [(0, 1)]))
    with pytest.raises(ValueError, match="ImageSize"):
        bmap.verify_image(bm, gz(tmp_path, holes(image(17), [(0, 1)])))
    with pytest.raises(ValueError, match="ImageSize"):
        bmap.verify_image(bm, gz(tmp_path, holes(image(8), [(0, 1)])))


# --- the board-side script ---------------------------------------------------------

def board(args: list[str], stdin: bytes = b"") -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(BOARD_SCRIPT), *args], input=stdin,
                          capture_output=True, timeout=60)


def spans_file(tmp_path: Path, bm: bmap.Bmap) -> Path:
    p = tmp_path / "spans"
    p.write_text(bmap.spans_text(bm), encoding="ascii")
    return p


def test_board_write_touches_only_the_mapped_spans(tmp_path):
    img = image(16, tail=100)
    bm = bmap.parse(bmap_xml(img, [(0, 1), (5, 5), (16, 16)]))
    dev = tmp_path / "emmc"
    dev.write_bytes(b"\xee" * (20 * BLOCK))
    r = board(["write", str(dev), str(spans_file(tmp_path, bm)), str(len(img))], stdin=img)
    assert r.returncode == 0, r.stderr
    got = dev.read_bytes()
    assert len(got) == 20 * BLOCK                                  # never truncated
    assert got[:2 * BLOCK] == img[:2 * BLOCK]
    assert got[2 * BLOCK:5 * BLOCK] == b"\xee" * (3 * BLOCK)        # unmapped: untouched
    assert got[5 * BLOCK:6 * BLOCK] == img[5 * BLOCK:6 * BLOCK]
    assert got[6 * BLOCK:16 * BLOCK] == b"\xee" * (10 * BLOCK)
    assert got[16 * BLOCK:16 * BLOCK + 100] == img[16 * BLOCK:]
    assert got[16 * BLOCK + 100:] == b"\xee" * (4 * BLOCK - 100)


def test_board_write_fails_on_a_stream_shorter_than_the_image(tmp_path):
    img = image(16)
    bm = bmap.parse(bmap_xml(img, [(0, 1), (9, 12)]))
    dev = tmp_path / "emmc"
    dev.write_bytes(b"\xee" * len(img))
    r = board(["write", str(dev), str(spans_file(tmp_path, bm)), str(len(img))], stdin=img[:10 * BLOCK])
    assert r.returncode != 0
    assert b"stream ended" in r.stderr
    # a truncated stream that covers every span still fails: the tail is counted
    r = board(["write", str(dev), str(spans_file(tmp_path, bm)), str(len(img))], stdin=img[:14 * BLOCK])
    assert r.returncode != 0 and b"stream ended" in r.stderr


def test_board_write_fails_on_a_stream_longer_than_the_image(tmp_path):
    img = image(16)
    bm = bmap.parse(bmap_xml(img, [(0, 1)]))
    dev = tmp_path / "emmc"
    dev.write_bytes(b"\xee" * len(img))
    r = board(["write", str(dev), str(spans_file(tmp_path, bm)), str(len(img))], stdin=img + b"x")
    assert r.returncode != 0 and b"longer" in r.stderr


def test_board_read_emits_the_mapped_bytes_in_order(tmp_path):
    img = image(16, tail=100)
    bm = bmap.parse(bmap_xml(img, [(0, 1), (5, 5), (16, 16)]))
    dev = tmp_path / "emmc"
    dev.write_bytes(img + b"\xee" * 5000)
    r = board(["read", str(dev), str(spans_file(tmp_path, bm))])
    assert r.returncode == 0, r.stderr
    assert r.stdout == img[:2 * BLOCK] + img[5 * BLOCK:6 * BLOCK] + img[16 * BLOCK:]


def test_board_read_fails_on_a_device_shorter_than_a_span(tmp_path):
    img = image(16)
    bm = bmap.parse(bmap_xml(img, [(9, 12)]))
    dev = tmp_path / "emmc"
    dev.write_bytes(img[:10 * BLOCK])
    r = board(["read", str(dev), str(spans_file(tmp_path, bm))])
    assert r.returncode != 0 and b"short read" in r.stderr


def test_board_script_imports_only_os_and_sys():
    # The unit's rootfs carries python3-core; hashlib and xml are separate
    # Yocto packages, so the board side must not need them.
    imports = [ln.strip() for ln in BOARD_SCRIPT.read_text(encoding="utf-8").splitlines()
               if ln.startswith(("import ", "from "))]
    assert imports == ["import os", "import sys"]


# --- linux_target: the same script, driven over (a local stand-in for) SSH -----------

class LocalUnit:
    """subprocess.run stand-in that runs the remote command on this host:
    scp copies the file, the unit's /tmp is a directory under tmp_path, and
    the partition-table re-read (meaningless on a plain file) is a no-op."""

    def __init__(self, tmp_path: Path, python_rc: int | None = None):
        self.tmp = tmp_path / "unit-tmp"
        self.tmp.mkdir(exist_ok=True)
        self.python_rc = python_rc
        self.commands: list[str] = []

    def __call__(self, argv, stdin=None, **kw):
        if argv[0] == "scp":
            remote = argv[-1].split(":", 1)[1].replace("/tmp/", f"{self.tmp}/")
            Path(remote).write_bytes(Path(argv[-2]).read_bytes())
            return subprocess.CompletedProcess(argv, 0, "", "")
        cmd = argv[-1]
        self.commands.append(cmd)
        if "rereadpt" in cmd:
            return subprocess.CompletedProcess(argv, 0, "", "")
        if self.python_rc is not None and cmd.startswith("python3 -c"):
            return subprocess.CompletedProcess(argv, self.python_rc, "", "sh: python3: not found")
        local = cmd.replace("/tmp/", f"{self.tmp}/").replace("python3", sys.executable)
        return subprocess.run(["sh", "-c", local], stdin=stdin, capture_output=True, text=True,
                              encoding="utf-8", timeout=60)


def unit(tmp_path, **kw):
    from provision import linux_target as lt
    runner = LocalUnit(tmp_path, **kw)
    return lt, lt.LinuxTarget("unit", "root", runner=runner), runner


def test_rootfs_bmap_write_verify_writes_only_the_mapped_spans(tmp_path):
    lt, t, runner = unit(tmp_path)
    img = holes(image(16, tail=100), [(0, 1), (5, 5), (16, 16)])
    bm = bmap.parse(bmap_xml(img, [(0, 1), (5, 5), (16, 16)]))
    wic = gz(tmp_path, img)
    want = bmap.verify_image(bm, wic)
    dev = tmp_path / "emmc"
    dev.write_bytes(b"\xee" * (20 * BLOCK))
    assert lt.rootfs_bmap_write_verify(t, str(dev), wic, bm, want) == want
    got = dev.read_bytes()
    assert got[:2 * BLOCK] == img[:2 * BLOCK] and got[5 * BLOCK:6 * BLOCK] == img[5 * BLOCK:6 * BLOCK]
    assert got[2 * BLOCK:5 * BLOCK] == b"\xee" * (3 * BLOCK)
    assert got[16 * BLOCK:16 * BLOCK + 100] == img[16 * BLOCK:]
    assert not any(" dd " in c or c.startswith("dd ") for c in runner.commands)   # no full-image dd
    assert any("rereadpt" in c for c in runner.commands)


def test_rootfs_bmap_write_verify_raises_when_the_readback_differs(tmp_path):
    lt, t, _ = unit(tmp_path)
    img = image(16)
    bm = bmap.parse(bmap_xml(img, [(0, 1)]))
    dev = tmp_path / "emmc"
    dev.write_bytes(b"\xee" * len(img))
    with pytest.raises(lt.BenchError, match="readback md5"):
        lt.rootfs_bmap_write_verify(t, str(dev), gz(tmp_path, img), bm, "0" * 32)


def test_rootfs_bmap_write_verify_raises_on_a_truncated_stream(tmp_path):
    lt, t, _ = unit(tmp_path)
    img = image(16)
    bm = bmap.parse(bmap_xml(img, [(0, 1), (9, 12)]))
    dev = tmp_path / "emmc"
    dev.write_bytes(b"\xee" * len(img))
    with pytest.raises(lt.BenchError, match="stream ended"):
        lt.rootfs_bmap_write_verify(t, str(dev), gz(tmp_path, img[:10 * BLOCK]), bm, "0" * 32)


def test_bmap_md5_ignores_what_sits_in_the_unmapped_blocks(tmp_path):
    lt, t, _ = unit(tmp_path)
    img = holes(image(16), [(0, 1), (9, 12)])
    bm = bmap.parse(bmap_xml(img, [(0, 1), (9, 12)]))
    held = bytearray(img)
    held[3 * BLOCK:8 * BLOCK] = b"\xaa" * (5 * BLOCK)     # stale data between the ranges
    dev = tmp_path / "emmc"
    dev.write_bytes(bytes(held))
    assert lt.bmap_md5(t, str(dev), bm) == bmap.verify_image(bm, gz(tmp_path, img))
    held[10 * BLOCK] ^= 0xFF                              # a mapped byte differs
    dev.write_bytes(bytes(held))
    assert lt.bmap_md5(t, str(dev), bm) != bmap.verify_image(bm, gz(tmp_path, img))


def test_has_python3_reports_a_unit_without_the_interpreter(tmp_path):
    lt, t, _ = unit(tmp_path, python_rc=127)
    assert lt.has_python3(t) is False
    lt, t, _ = unit(tmp_path)
    assert lt.has_python3(t) is True


# --- the bundle role, the gates and the write_rootfs step ----------------------------

import json  # noqa: E402
import re  # noqa: E402

import provision_som  # noqa: E402
from provision import gates, steps  # noqa: E402

from .test_check_som_bundle import _run as check_bundle  # noqa: E402
from .test_check_som_bundle import _valid_bundle  # noqa: E402
from .test_provision_steps import Board, _bench, _bundle, _ctx, _failed, _statuses  # noqa: E402

BMAP_ROLE = "system_image_bmap"
EMMC = "/dev/mmcblk0"


def data_ranges(img: bytes) -> list[tuple[int, int]]:
    """Block ranges of `img` that hold data: what bmaptool would map."""
    ranges: list[tuple[int, int]] = []
    for b in range(-(-len(img) // BLOCK)):
        if any(img[b * BLOCK:(b + 1) * BLOCK]):
            if ranges and ranges[-1][1] == b - 1:
                ranges[-1] = (ranges[-1][0], b)
            else:
                ranges.append((b, b))
    return ranges


def bmap_bundle(tmp_path, xml=None):
    """The step-test bundle plus a system_image_bmap side-car. Returns
    ((bundle_dir, bundle), image bytes, mapped block ranges)."""
    bdir, b = _bundle(tmp_path)
    img = gzip.decompress((bdir / "artifacts" / "img.wic.gz").read_bytes())
    ranges = data_ranges(img)
    assert len(ranges) >= 2, "the fixture wic has no hole between two data ranges"
    data = (xml or bmap_xml(img, ranges)).encode()
    (bdir / "artifacts" / "img.wic.bmap").write_bytes(data)
    b["components"].append({"role": BMAP_ROLE, "file": "artifacts/img.wic.bmap",
                            "sha256": hashlib.sha256(data).hexdigest(), "size_bytes": len(data),
                            "flash_target": "none"})
    (bdir / "bundle.json").write_text(json.dumps(b), encoding="utf-8")
    return (bdir, b), img, ranges


class BmapBoard(Board):
    """Board whose unit side models board_bmap.py (the real script is covered
    above through LocalUnit) and a rootfs that passes fsck + the dtb check."""

    def __init__(self, python3=True, **kw):
        super().__init__(**kw)
        self.python3 = python3
        self.stdin_path = None

    def put(self, local, remote):
        self.files[remote] = Path(local).read_bytes()

    def run(self, cmd, timeout=60.0, check=True, stdin_path=None):
        self.stdin_path = stdin_path
        res = super().run(cmd, timeout, check, stdin_path)
        if res.rc == 0 and " read " in cmd and "alp-board-bmap" in cmd:
            total = sum(e - s for s, e in self._spans())
            return type(res)(res.rc, res.stdout, f"board_bmap: read {total} bytes\n")
        return res

    def _spans(self):
        return [tuple(int(x) for x in ln.split())
                for ln in self.files["/tmp/alp-board-bmap.spans"].decode().splitlines()]

    def _answer(self, cmd):
        if cmd == "python3 -c ''":
            return (0 if self.python3 else 127), ""
        if m := re.match(r"python3 /tmp/alp-board-bmap\.py read (\S+) \S+ \| md5sum$", cmd):
            data = self.files[m[1]]
            return 0, hashlib.md5(b"".join(data[s:e] for s, e in self._spans())).hexdigest() + "  -\n"
        if m := re.match(r"gunzip -c \| python3 /tmp/alp-board-bmap\.py write (\S+) \S+ (\d+) && sync$", cmd):
            img = gzip.decompress(Path(self.stdin_path).read_bytes())
            assert len(img) == int(m[2])
            dev = bytearray(self.files[m[1]])
            for s, e in self._spans():
                dev[s:e] = img[s:e]
            self.files[m[1]] = bytes(dev)
            return 0, ""
        if "rereadpt" in cmd:
            return 0, ""
        if cmd.startswith("ls -d /sys/block/mmcblk0/"):
            return 0, "/sys/block/mmcblk0/mmcblk0p1\n/sys/block/mmcblk0/mmcblk0p2\n"
        if re.match(r"fsck\.ext4 -n|mkdir -p \S+ && mount -o ro|test -f|umount", cmd):
            return 0, ""
        return super()._answer(cmd)


def unmapped(img, ranges):
    """Byte bounds of the first hole (between the first two data ranges)."""
    return [(ranges[0][1] + 1) * BLOCK, ranges[1][0] * BLOCK]


def mapped_bytes(img, ranges):
    return sum(min((last + 1) * BLOCK, len(img)) - first * BLOCK for first, last in ranges)


def test_write_rootfs_with_a_bmap_writes_only_the_mapped_blocks(tmp_path):
    bundle, img, ranges = bmap_bundle(tmp_path)
    board = BmapBoard(emmc=b"\xee" * (len(img) + BLOCK))
    ctx = _ctx(tmp_path, bundle=bundle, bench=_bench(), linux=board, execute=True)
    res = steps.run_steps(ctx, only=["write_rootfs"])
    assert _statuses(res) == {"preflight": "done", "write_rootfs": "done"}, [(r.name, r.detail) for r in res]
    got = board.files[EMMC]
    lo, hi = unmapped(img, ranges)
    for first, last in ranges:
        assert got[first * BLOCK:(last + 1) * BLOCK][:len(img) - first * BLOCK] == img[first * BLOCK:(last + 1) * BLOCK]
    assert got[lo:hi] == b"\xee" * (hi - lo)                      # unmapped blocks are not written
    assert not any(re.search(r"\bdd\b[^|]*\bof=", c) for c in board.commands)


def test_write_rootfs_probe_accepts_an_emmc_whose_mapped_blocks_match(tmp_path):
    bundle, img, ranges = bmap_bundle(tmp_path)
    held = bytearray(img)
    lo, hi = unmapped(img, ranges)
    held[lo:hi] = b"\xaa" * (hi - lo)
    board = BmapBoard(emmc=bytes(held))
    ctx = _ctx(tmp_path, bundle=bundle, bench=_bench(), linux=board, execute=True)
    res = steps.run_steps(ctx, only=["write_rootfs"])
    assert _statuses(res)["write_rootfs"] == "skipped"
    assert not any("write" in c for c in board.commands)


def test_write_rootfs_writes_the_full_image_when_the_unit_has_no_python3(tmp_path):
    bundle, img, _ = bmap_bundle(tmp_path)
    board = BmapBoard(python3=False, emmc=b"\xee" * (len(img) + BLOCK))
    ctx = _ctx(tmp_path, bundle=bundle, bench=_bench(), linux=board, execute=True)
    steps.run_steps(ctx, only=["write_rootfs"])
    assert f"gunzip -c | dd of={EMMC} bs=4M && sync" in board.commands
    assert not any("alp-board-bmap" in c for c in board.commands)
    assert any("no python3" in line for line in ctx.plan_log)


def test_dry_run_plans_the_mapped_block_write(tmp_path):
    bundle, img, ranges = bmap_bundle(tmp_path)
    ctx = _ctx(tmp_path, bundle=bundle, bench=_bench())
    res = steps.run_steps(ctx, only=["write_rootfs"])
    assert _statuses(res)["write_rootfs"] == "planned"
    mapped = mapped_bytes(img, ranges)
    assert any(line.startswith("WOULD: ") and f"{mapped} of {len(img)} bytes" in line for line in ctx.plan_log)


def test_preflight_refuses_a_bmap_built_from_another_image(tmp_path):
    bdir, _b = _bundle(tmp_path / "probe")
    img = gzip.decompress((bdir / "artifacts" / "img.wic.gz").read_bytes())
    other = bytes(b ^ 0xFF for b in img)
    bundle, _, _ = bmap_bundle(tmp_path, xml=bmap_xml(other, [(0, 0)]))
    res = steps.run_steps(_ctx(tmp_path, bundle=bundle), only=["write_rootfs"])
    bad = _failed(res)
    assert bad.name == "preflight" and "bmap" in bad.detail and "blocks 0-0" in bad.detail


def test_preflight_reports_the_bmap_gate_when_the_bundle_ships_one(tmp_path):
    bundle, _, _ = bmap_bundle(tmp_path)
    ctx = _ctx(tmp_path, bundle=bundle)
    steps.run_steps(ctx, only=["preflight"])
    assert "ok  bmap:" in ctx.step_logs["preflight"]
    plain = _ctx(tmp_path / "plain")
    steps.run_steps(plain, only=["preflight"])
    assert "bmap" not in plain.step_logs["preflight"]


def test_artefacts_gate_checks_the_bmap_like_any_component(tmp_path):
    (bdir, b), _, _ = bmap_bundle(tmp_path)
    assert gates.artefacts(bdir, b).ok
    (bdir / "artifacts" / "img.wic.bmap").write_bytes(b"tampered")
    assert not gates.artefacts(bdir, b).ok


def _schema_bundle(tmp_path, role=BMAP_ROLE, target="none"):
    doc = _valid_bundle()
    doc["components"].append({"role": role, "file": "artifacts/img.wic.bmap", "sha256": "0" * 64,
                              "size_bytes": 1, "flash_target": target})
    p = tmp_path / "bundle.json"
    p.write_text(json.dumps(doc), encoding="utf-8")
    return p


def test_schema_accepts_the_optional_bmap_role(tmp_path):
    proc = check_bundle("--bundle", str(_schema_bundle(tmp_path)))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_schema_rejects_a_bmap_with_a_flash_target(tmp_path):
    assert check_bundle("--bundle", str(_schema_bundle(tmp_path, target="emmc"))).returncode != 0


def test_schema_rejects_flash_target_none_on_a_flashed_role(tmp_path):
    doc = _valid_bundle()
    doc["components"][2]["flash_target"] = "none"
    p = tmp_path / "bundle.json"
    p.write_text(json.dumps(doc), encoding="utf-8")
    assert check_bundle("--bundle", str(p)).returncode != 0


def test_legacy_flow_does_not_flash_the_bmap(tmp_path):
    step = provision_som._flash(provision_som.Cfg(bundle_dir=tmp_path, execute=True),
                                {"role": BMAP_ROLE, "file": "img.wic.bmap", "flash_target": "none"})
    assert step.ok and "not flashed" in step.message and step.command == []


def _build_dir(tmp_path):
    d = tmp_path / "deploy"
    d.mkdir()
    for name in ("bl2_bp_spi.bin", "bl2_bp_mmc.bin", "fip.bin", "alp-image-edge.wic.gz"):
        (d / name).write_bytes(b"x")
    return d


def test_build_dir_picks_up_a_bmap_beside_the_wic(tmp_path):
    d = _build_dir(tmp_path)
    roles = [c["role"] for c in provision_som._bundle_from_build_dir(d)["components"]]
    assert BMAP_ROLE not in roles
    (d / "alp-image-edge.wic.bmap").write_bytes(b"<bmap/>")
    comp = provision_som._bundle_from_build_dir(d)["components"][-1]
    assert comp["role"] == BMAP_ROLE and comp["file"] == "alp-image-edge.wic.bmap"
    assert comp["flash_target"] == "none"


def test_build_dir_refuses_two_bmaps(tmp_path):
    d = _build_dir(tmp_path)
    (d / "a.wic.bmap").write_bytes(b"1")
    (d / "b.wic.bmap").write_bytes(b"2")
    with pytest.raises(ValueError, match=r"\*\.wic\.bmap"):
        provision_som._bundle_from_build_dir(d)


# --- review fixes --------------------------------------------------------------------

def sparse_image(blocks: int, mapped: list[tuple[int, int]]) -> bytes:
    """Zero everywhere except the mapped block ranges (what a real bmap describes)."""
    img = bytearray(blocks * BLOCK)
    for first, last in mapped:
        for b in range(first, last + 1):
            img[b * BLOCK:(b + 1) * BLOCK] = bytes([b + 1]) * BLOCK
    return bytes(img)


def test_verify_image_rejects_a_bmap_that_leaves_data_blocks_out(tmp_path):
    # Data in block 9, but the bmap lists only blocks 0-1: every listed range
    # matches, and a unit written from it would lack block 9.
    img = sparse_image(16, [(0, 1), (9, 9)])
    bm = bmap.parse(bmap_xml(img, [(0, 1)]))
    with pytest.raises(ValueError, match="outside the mapped ranges"):
        bmap.verify_image(bm, gz(tmp_path, img))


def test_verify_image_accepts_holes_between_and_after_the_ranges(tmp_path):
    img = sparse_image(16, [(0, 1), (9, 9)])
    bm = bmap.parse(bmap_xml(img, [(0, 1), (9, 9)]))
    assert bmap.verify_image(bm, gz(tmp_path, img)) == hashlib.md5(img[:2 * BLOCK] + img[9 * BLOCK:10 * BLOCK]).hexdigest()


def test_parse_rejects_a_mapped_blocks_count_that_disagrees_with_the_ranges():
    xml = bmap_xml(image(), [(0, 1)])
    body = re.sub(r"[0-9a-f]{64}(?= </BmapFileChecksum>)", "0" * 64,
                  xml.replace("<MappedBlocksCount> 2 ", "<MappedBlocksCount> 9 "))
    xml = body.replace("0" * 64, hashlib.sha256(body.encode()).hexdigest(), 1)
    with pytest.raises(ValueError, match="MappedBlocksCount"):
        bmap.parse(xml)


def test_parse_rejects_a_doctype_declaration():
    with pytest.raises(ValueError, match="DOCTYPE"):
        bmap.parse('<!DOCTYPE bmap [<!ENTITY a "b">]>' + bmap_xml(image(), [(0, 1)]))


def test_parse_rejects_a_backwards_range_and_a_bad_range_checksum():
    xml = bmap_xml(image(), [(4, 6)])
    for old, new, why in (("> 4-6 <", "> 6-4 <", "backwards"), ('chksum="', 'chksum="zz', "unreadable")):
        body = re.sub(r"[0-9a-f]{64}(?= </BmapFileChecksum>)", "0" * 64, xml.replace(old, new))
        with pytest.raises(ValueError, match=why):
            bmap.parse(body.replace("0" * 64, hashlib.sha256(body.encode()).hexdigest(), 1))


def test_bmap_md5_raises_when_the_unit_side_reader_fails(tmp_path):
    # `reader | md5sum` exits 0 whatever the reader did; a short device must
    # still surface as a reader failure, not as a digest of partial output.
    lt, t, _ = unit(tmp_path)
    img = image(16)
    bm = bmap.parse(bmap_xml(img, [(9, 12)]))
    dev = tmp_path / "emmc"
    dev.write_bytes(img[:10 * BLOCK])
    with pytest.raises(lt.BenchError, match="short read"):
        lt.bmap_md5(t, str(dev), bm)


def test_board_read_reports_the_byte_count_on_stderr(tmp_path):
    img = image(16)
    bm = bmap.parse(bmap_xml(img, [(0, 1), (9, 12)]))
    dev = tmp_path / "emmc"
    dev.write_bytes(img)
    r = board(["read", str(dev), str(spans_file(tmp_path, bm))])
    assert r.returncode == 0 and b"read 24576 bytes" in r.stderr


def test_has_python3_raises_on_a_failure_that_is_not_a_missing_interpreter(tmp_path):
    lt, t, _ = unit(tmp_path, python_rc=255)        # e.g. the ssh connection dropped
    with pytest.raises(lt.BenchError, match="python3"):
        lt.has_python3(t)


def test_write_rootfs_result_names_the_mapped_block_write(tmp_path):
    bundle, img, _ = bmap_bundle(tmp_path)
    board_ = BmapBoard(emmc=b"\xee" * (len(img) + BLOCK))
    ctx = _ctx(tmp_path, bundle=bundle, bench=_bench(), linux=board_, execute=True)
    res = steps.run_steps(ctx, only=["write_rootfs"])
    assert "mapped blocks only" in res[-1].detail
