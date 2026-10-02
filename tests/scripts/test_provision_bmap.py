"""bmap-aware write_rootfs: only the mapped ranges are written and verified.
The wic is the synthetic sparse image from test_provision_gates (a 1 MiB
zero gap, then a 16 KiB ext4); the bmap is generated here from its bytes."""

from __future__ import annotations

import gzip
import hashlib
import json
import re
import shutil
import subprocess

import pytest
from provision import bmap, steps
from provision import linux_target as lt
from provision.bench import BenchError

from .test_provision_gates import _ext4, _wic
from .test_provision_steps import DTB, WritableBoard, _bench, _bundle, _ctx

BS = 4096
STALE = 0xEE


def make_bmap_xml(raw: bytes, bs: int = BS, ctype: str = "sha256", corrupt: bool = False) -> str:
    """A bmap for raw: every block holding a non-zero byte is mapped, adjacent blocks merged."""
    nblocks = -(-len(raw) // bs)
    used = [i for i in range(nblocks) if any(raw[i * bs:(i + 1) * bs])]
    runs: list[list[int]] = []
    for i in used:
        if runs and runs[-1][1] == i - 1:
            runs[-1][1] = i
        else:
            runs.append([i, i])
    body = ""
    for a, b in runs:
        h = hashlib.new(ctype, raw[a * bs:(b + 1) * bs]).hexdigest()
        if corrupt:
            h = "0" * len(h)
        span = f"{a}-{b}" if a != b else f"{a}"
        body += f'    <Range chksum="{h}"> {span} </Range>\n'
    return (f'<?xml version="1.0" ?>\n<bmap version="2.0">\n <ImageSize> {len(raw)} </ImageSize>\n'
            f' <BlockSize> {bs} </BlockSize>\n <BlocksCount> {nblocks} </BlocksCount>\n'
            f' <MappedBlocksCount> {len(used)} </MappedBlocksCount>\n'
            f' <ChecksumType> {ctype} </ChecksumType>\n <BlockMap>\n{body} </BlockMap>\n</bmap>\n')


RAW = _wic(_ext4(["Image", DTB]))


def with_bmap(tmp_path, **kw):
    bdir, bundle = _bundle(tmp_path)
    raw = gzip.decompress((bdir / "artifacts" / "img.wic.gz").read_bytes())
    assert raw == RAW
    xml = make_bmap_xml(raw, **kw).encode()
    (bdir / "artifacts" / "img.wic.bmap").write_bytes(xml)
    bundle["components"].append({"role": "system_image_bmap", "file": "artifacts/img.wic.bmap",
                                 "sha256": hashlib.sha256(xml).hexdigest(), "size_bytes": len(xml),
                                 "flash_target": "emmc"})
    (bdir / "bundle.json").write_text(json.dumps(bundle), encoding="utf-8")
    return bdir, bundle


class BmapBoard(WritableBoard):
    """eMMC starts as STALE bytes; the gunzip|dd loop and the range md5 are interpreted."""

    def __init__(self, fullblock=True, flip_readback=False, **kw):
        super().__init__(**kw)
        self.files["/dev/mmcblk0"] = bytearray([STALE]) * (len(RAW) + BS)
        self.fullblock, self.flip, self.written = fullblock, flip_readback, 0

    def _ranges(self):
        return [tuple(map(int, ln.split())) for ln in self.files[lt.RANGES_PATH].decode().splitlines()]

    def run(self, cmd, timeout=60.0, check=True, stdin_path=None):
        if cmd.startswith("gunzip -c | { while read"):
            self.commands.append(cmd)
            m = re.search(r"of=(\S+) bs=(\d+)", cmd)
            data, dev, bs = gzip.decompress(stdin_path.read_bytes()), self.files[m[1]], int(m[2])
            pos = 0
            for s, c in self._ranges():
                dev[s * bs:(s + c) * bs] = data[pos:pos + c * bs]
                pos += c * bs
            self.written = pos
            assert pos == len(data)
            return lt.CmdResult(0, "", "")
        return super().run(cmd, timeout, check, stdin_path)

    def _answer(self, cmd):
        if cmd.startswith("dd if=/dev/zero") and "iflag=fullblock" in cmd:
            return (0 if self.fullblock else 1), ""
        if cmd.startswith(("blockdev --rereadpt", "fsck.ext4", "umount", "test -f", "mkdir -p")):
            return 0, ""
        if m := re.match(r"ls -d /sys/block/(\w+)/", cmd):
            return 0, f"/sys/block/{m[1]}/{m[1]}p2\n"
        if m := re.search(r"dd if=(\S+) bs=(\d+) skip=\$s", cmd):
            dev, bs = self.files[m[1]], int(m[2])
            out = b"".join(bytes(dev[s * bs:(s + c) * bs]) for s, c in self._ranges())
            if self.flip:
                out = b"\xff" + out[1:]
            return 0, hashlib.md5(out).hexdigest() + "  -\n"
        return super()._answer(cmd)


def _step(tmp_path, board, **kw):
    ctx = _ctx(tmp_path, bundle=with_bmap(tmp_path, **kw), bench=_bench(), linux=board, execute=True)
    return ctx, steps.run_steps(ctx, only=["write_rootfs"])[-1]


def test_only_mapped_ranges_written(tmp_path):
    board = BmapBoard()
    ctx, r = _step(tmp_path, board)
    assert r.status == "done", r.detail
    dev = board.files["/dev/mmcblk0"]
    bm = bmap.parse(ctx.artefact("system_image_bmap"))
    assert 0 < bm.mapped_bytes < len(RAW) // 2
    assert board.written == bm.mapped_bytes
    for s, c, _ in bm.ranges:                               # mapped: equal to the image
        assert dev[s * BS:(s + c) * BS] == RAW[s * BS:(s + c) * BS].ljust(c * BS, b"\0")
    assert dev[BS:256 * BS] == bytes([STALE]) * (255 * BS)  # the zero gap was never touched
    assert r.evidence["rootfs_bytes_written"] == str(bm.mapped_bytes)
    assert r.evidence["rootfs_image_bytes"] == str(len(RAW))
    assert not any(c.startswith("gunzip -c | dd") for c in board.commands)
    assert any(c.endswith("&& sync") and "iflag=fullblock" in c for c in board.commands)


def test_mount_is_read_only_noload(tmp_path):
    board = BmapBoard()
    _step(tmp_path, board)
    mounts = [c for c in board.commands if "mount -o" in c]
    assert mounts and all("-o ro,noload " in c for c in mounts)


def test_bmap_checksum_mismatch_refuses_before_any_write(tmp_path):
    board = BmapBoard()
    _, r = _step(tmp_path, board, corrupt=True)
    assert r.status == "failed" and "nothing was written" in r.detail
    assert not any("dd of=" in c or c.startswith("gunzip") for c in board.commands)
    assert board.files["/dev/mmcblk0"] == bytes([STALE]) * (len(RAW) + BS)


def test_sha1_bmap_is_accepted(tmp_path):
    _, r = _step(tmp_path, BmapBoard(), ctype="sha1")
    assert r.status == "done", r.detail


def test_readback_mismatch_is_detected(tmp_path):
    _, r = _step(tmp_path, BmapBoard(flip_readback=True))
    assert r.status == "failed" and "readback md5" in r.detail


def test_no_bmap_falls_back_to_full_dd(tmp_path, monkeypatch):
    board = BmapBoard()
    calls = []
    monkeypatch.setattr(lt, "rootfs_write_verify", lambda *a, **k: calls.append(a) or "md5")
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, execute=True)   # plain bundle: no bmap role
    steps.run_steps(ctx, only=["write_rootfs"])
    assert len(calls) == 1
    assert not any("while read" in c for c in board.commands)


def test_dd_without_fullblock_falls_back_to_full_image(tmp_path, monkeypatch):
    board = BmapBoard(fullblock=False)
    calls = []
    monkeypatch.setattr(lt, "rootfs_write_verify", lambda *a, **k: calls.append(a) or "md5")
    _step(tmp_path, board)
    assert len(calls) == 1 and not any(c.startswith("gunzip -c | {") for c in board.commands)


def test_probe_satisfied_only_when_mapped_ranges_match(tmp_path):
    board = BmapBoard()
    ctx = _ctx(tmp_path, bundle=with_bmap(tmp_path), bench=_bench(), linux=board, execute=True)
    assert isinstance(steps.WriteRootfs().probe(ctx), steps.Unsatisfied)
    steps.run_steps(ctx, only=["write_rootfs"])
    ctx2 = _ctx(tmp_path / "b", bundle=with_bmap(tmp_path / "b"), bench=_bench(), linux=board, execute=True)
    assert isinstance(steps.WriteRootfs().probe(ctx2), steps.Satisfied)
    board.files["/dev/mmcblk0"][256 * BS] ^= 0xFF          # corrupt a mapped block
    ctx3 = _ctx(tmp_path / "c", bundle=with_bmap(tmp_path / "c"), bench=_bench(), linux=board, execute=True)
    assert isinstance(steps.WriteRootfs().probe(ctx3), steps.Unsatisfied)


def test_parse_rejects_garbage(tmp_path):
    p = tmp_path / "x.bmap"
    p.write_text("<bmap><BlockSize>4096</BlockSize></bmap>", encoding="utf-8")
    with pytest.raises(BenchError, match="not a valid bmap"):
        bmap.parse(p)


# --- the generated shell commands, run by a real sh/dd/gzip -------------------------------------
# The fakes above only model the gunzip|dd loops; this harness executes them.

needs_sh = pytest.mark.skipif(not (shutil.which("sh") and shutil.which("dd") and shutil.which("gzip")),
                              reason="needs a POSIX sh with dd and gzip")


class RealShell:
    """A target whose run() is `sh -c` in a scratch dir: the device is the file dev.img."""

    def __init__(self, work, monkeypatch):
        self.work = work
        work.mkdir(exist_ok=True)
        monkeypatch.setattr(lt, "RANGES_PATH", "ranges.txt")

    def run(self, cmd, timeout=60.0, check=True, stdin_path=None, **kw):
        if cmd.startswith("blockdev"):
            return lt.CmdResult(0, "", "")
        stdin = open(stdin_path, "rb") if stdin_path else subprocess.DEVNULL
        try:
            p = subprocess.run(["sh", "-c", cmd], cwd=self.work, capture_output=True, text=True,
                               timeout=120, stdin=stdin)
        finally:
            if stdin_path:
                stdin.close()
        if check and p.returncode:
            raise BenchError(f"{cmd}: {p.stderr}")
        return lt.CmdResult(p.returncode, p.stdout, p.stderr)

    def put(self, local, remote):
        shutil.copy(local, self.work / remote)


def make_wic(tmp_path, raw: bytes):
    """(wic.gz path, parsed bmap) for raw, mapping every non-zero block."""
    wic = tmp_path / "x.wic.gz"
    wic.write_bytes(gzip.compress(raw))
    xml = tmp_path / "x.wic.bmap"
    xml.write_text(make_bmap_xml(raw), encoding="utf-8")
    return wic, bmap.parse(xml)


@needs_sh
def test_host_stream_write_in_a_real_shell(tmp_path, monkeypatch):
    """No store: the host sends only the mapped bytes, the board's gunzip|dd loop places them."""
    wic, bm = make_wic(tmp_path, RAW)
    sh = RealShell(tmp_path / "w", monkeypatch)
    (sh.work / "dev.img").write_bytes(bytes([STALE]) * (len(RAW) + BS))
    ev = lt.rootfs_write_verify_mapped(sh, "dev.img", wic, bm)
    dev = (sh.work / "dev.img").read_bytes()
    for s, c, _ in bm.ranges:
        assert dev[s * BS:(s + c) * BS] == RAW[s * BS:(s + c) * BS].ljust(c * BS, b"\0")
    assert dev[BS:256 * BS] == bytes([STALE]) * (255 * BS)      # the gap was never touched
    assert ev["rootfs_bytes_written"] == str(bm.mapped_bytes)


@needs_sh
@pytest.mark.parametrize("stored", [False, True])
def test_image_not_a_whole_number_of_blocks_pads_the_last_block_on_both_paths(tmp_path, monkeypatch, stored):
    raw = RAW + b"\x07" * 100                    # ImageSize % BlockSize != 0, the tail is data
    assert len(raw) % BS
    wic, bm = make_wic(tmp_path, raw)
    assert bm.image_size == len(raw)
    sh = RealShell(tmp_path / "w", monkeypatch)
    (sh.work / "dev.img").write_bytes(bytes([STALE]) * (len(raw) + 2 * BS))
    store = None
    if stored:
        shutil.copy(wic, sh.work / "payload.gz")
        store = type("S", (), {"fetch": lambda self, local: "payload.gz"})()
    lt.rootfs_write_verify_mapped(sh, "dev.img", wic, bm, store=store)     # raises on a readback mismatch
    dev = (sh.work / "dev.img").read_bytes()
    last = len(raw) // BS
    assert dev[last * BS:(last + 1) * BS] == raw[last * BS:].ljust(BS, b"\0")    # the stale tail is zeroed


def _undermapped(tmp_path, raw: bytes, drop: str):
    xml = re.sub(rf'    <Range chksum="\w+"> {drop} </Range>\n', "", make_bmap_xml(raw))
    (tmp_path / "x.bmap").write_text(xml, encoding="utf-8")
    wic = tmp_path / "x.wic.gz"
    wic.write_bytes(gzip.compress(raw))
    return wic, bmap.parse(tmp_path / "x.bmap")


def test_unmapped_non_zero_block_is_refused(tmp_path):
    raw = b"\x01" * BS + b"\x02" * BS + bytes(BS) + b"\x03" * BS      # blocks 0-1 and 3 are data
    wic, bm = _undermapped(tmp_path, raw, "0-1")                         # the bmap forgets blocks 0-1
    with pytest.raises(BenchError, match="unmapped holds non-zero"):
        bmap.stage(wic, bm, None)


def test_non_zero_tail_after_the_last_range_is_refused(tmp_path):
    raw = b"\x01" * BS + bytes(BS) + b"\x02" * BS
    wic, bm = _undermapped(tmp_path, raw, "2")
    with pytest.raises(BenchError, match="unmapped holds non-zero"):
        bmap.stage(wic, bm, None)
