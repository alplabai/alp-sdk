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
import sys
import tempfile
import zlib
from pathlib import Path

import pytest
from provision import bmap, bmap_writer, steps
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
    """eMMC starts as STALE bytes; the on-board python3 writer (the real script, on a temp file
    standing in for the device) and the range md5 are interpreted."""

    def __init__(self, python3=True, flip_readback=False, **kw):
        super().__init__(**kw)
        self.files["/dev/mmcblk0"] = bytearray([STALE]) * (len(RAW) + BS)
        self.python3, self.flip, self.written = python3, flip_readback, 0

    def _ranges(self):
        return [tuple(map(int, ln.split())) for ln in self.files[lt.RANGES_PATH].decode().splitlines()]

    def run(self, cmd, timeout=60.0, check=True, stdin_path=None, long_running=False):
        m = re.match(rf"python3 {lt.WRITER_PATH} (\S+) (\d+) {lt.RANGES_PATH} (\d+)(?: < (\S+))?$", cmd)
        if g := re.match(r"gunzip -c \| dd of=(\S+) bs=4M && sync$", cmd):       # the full-image write
            self.commands.append(cmd)
            self.files[g[1]] = bytearray(gzip.decompress(stdin_path.read_bytes()))
            return lt.CmdResult(0, "", "")
        if not m:
            return super().run(cmd, timeout, check, stdin_path)
        self.commands.append(cmd)
        stream = stdin_path.read_bytes() if stdin_path else bytes(self.files[m[4]])
        with tempfile.TemporaryDirectory() as d:
            dev, rng = Path(d) / "dev", Path(d) / "ranges"
            dev.write_bytes(bytes(self.files[m[1]]))
            rng.write_bytes(self.files[lt.RANGES_PATH])
            p = subprocess.run([sys.executable, str(bmap_writer.__file__), str(dev), m[2], str(rng), m[3]],
                               input=stream, capture_output=True)
            assert p.returncode == 0, p.stderr
            self.files[m[1]] = bytearray(dev.read_bytes())
        self.written = sum(c for _, c in self._ranges()) * int(m[2])
        return lt.CmdResult(0, "", "")

    def _answer(self, cmd):
        if cmd == "command -v python3":
            return (0 if self.python3 else 1), ""
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
    assert r.evidence["rootfs_write_mode"] == "bmap-python" and "rootfs_bmap_fallback" not in r.evidence
    assert not any(c.startswith("gunzip -c | dd") or "fullblock" in c for c in board.commands)
    assert any(c.startswith(f"python3 {lt.WRITER_PATH} /dev/mmcblk0 {BS} ") for c in board.commands)
    assert f"rm -f {lt.RANGES_PATH} {lt.WRITER_PATH}" in board.commands


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


def test_no_bmap_falls_back_to_full_dd(tmp_path):
    board = BmapBoard()
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, execute=True)   # plain bundle: no bmap role
    r = steps.run_steps(ctx, only=["write_rootfs"])[-1]
    assert r.status == "done", r.detail
    assert r.evidence["rootfs_write_mode"] == "full-image"
    assert r.evidence["rootfs_bmap_fallback"] == "the bundle has no system_image_bmap"
    assert r.evidence["rootfs_bytes_written"] == r.evidence["rootfs_image_bytes"] == str(len(RAW))
    assert not any(c.startswith("python3 ") and "bmap-writer" in c for c in board.commands)


def test_no_python3_falls_back_to_full_image_and_says_why(tmp_path):
    board = BmapBoard(python3=False)
    ctx, r = _step(tmp_path, board)
    assert r.status == "done", r.detail
    assert r.evidence["rootfs_write_mode"] == "full-image"
    assert r.evidence["rootfs_bmap_fallback"] == "the board has no python3"
    assert r.evidence["rootfs_bytes_written"] == str(len(RAW))
    assert any("gunzip -c" in c and "dd of=" in c for c in board.commands)
    assert lt.WRITER_PATH not in board.files
    assert any("no python3" in line for line in ctx.plan_log)


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
# The fakes above run the writer on a temp file; this harness runs the generated commands in sh.

needs_sh = pytest.mark.skipif(not (shutil.which("sh") and shutil.which("dd") and shutil.which("gzip")),
                              reason="needs a POSIX sh with dd and gzip")


class RealShell:
    """A target whose run() is `sh -c` in a scratch dir: the device is the file dev.img."""

    def __init__(self, work, monkeypatch):
        self.work = work
        work.mkdir(exist_ok=True)
        monkeypatch.setattr(lt, "RANGES_PATH", "ranges.txt")
        monkeypatch.setattr(lt, "WRITER_PATH", "writer.py")

    def run(self, cmd, timeout=60.0, check=True, stdin_path=None, **kw):
        if cmd.startswith("blockdev"):
            return lt.CmdResult(0, "", "")
        # The board's whole-system `sync` and page-cache drop are never run on the HOST: a global
        # sync waits for every dirty page of the machine (minutes on a CI host busy with other
        # builds), and as root the drop would empty the host's cache. The file written here is
        # read back through the same page cache either way.
        if cmd == "sync; echo 3 > /proc/sys/vm/drop_caches":
            return lt.CmdResult(0, "", "")
        cmd = re.sub(r"(?:^|(?<=;)|(?<=&&))\s*sync\s*(?=;|&&|$)", " true ", cmd)
        if cmd.startswith("python3 "):                  # the board's python3 is this interpreter
            cmd = f'"{sys.executable}" ' + cmd[len("python3 "):]
        stdin = open(stdin_path, "rb") if stdin_path else subprocess.DEVNULL
        try:
            p = subprocess.run(["sh", "-c", cmd], cwd=self.work, capture_output=True, text=True, encoding="utf-8",
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
    """No store: the host sends only the mapped bytes, the board's python3 writer places them."""
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
@pytest.mark.skipif(sys.platform == "win32", reason="a PATH-shadowed `sync` needs a POSIX PATH")
def test_the_real_shell_never_runs_the_hosts_global_sync(tmp_path, monkeypatch):
    """A whole-machine `sync` on a busy CI host took over 120 s (the gate on 2026-10-03): the
    harness must not run it. A `sync` that only leaves a mark shadows the real one."""
    import os
    bin_dir = tmp_path / "bin"
    bin_dir.mkdir()
    fake = bin_dir / "sync"
    fake.write_text('#!/bin/sh\ntouch "$SYNC_MARK"\n', encoding="utf-8", newline="\n")
    fake.chmod(0o755)
    monkeypatch.setenv("PATH", f"{bin_dir}{os.pathsep}{os.environ['PATH']}")
    monkeypatch.setenv("SYNC_MARK", str(tmp_path / "synced"))
    wic, bm = make_wic(tmp_path, RAW)
    sh = RealShell(tmp_path / "w", monkeypatch)
    (sh.work / "dev.img").write_bytes(bytes([STALE]) * (len(RAW) + BS))
    lt.rootfs_write_verify_mapped(sh, "dev.img", wic, bm)
    sh.run("gunzip -c x.gz 2>/dev/null | cat > /dev/null && sync", check=False)
    assert not (tmp_path / "synced").exists()


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


# --- bmap_writer.py itself, run as the board runs it (a temp file stands in for the device) ----

def _write(tmp_path, stream: bytes, ranges, image_size: int, dev_size: int, bs=BS):
    dev, rng, gz = tmp_path / "dev.img", tmp_path / "ranges", tmp_path / "in.gz"
    dev.write_bytes(bytes([STALE]) * dev_size)
    rng.write_text("".join(f"{s} {c}\n" for s, c in ranges), encoding="ascii")
    gz.write_bytes(stream)
    with open(gz, "rb") as f:
        p = subprocess.run([sys.executable, str(bmap_writer.__file__), str(dev), str(bs), str(rng),
                            str(image_size)], stdin=f, capture_output=True, text=True, encoding="utf-8")
    return p, dev.read_bytes()


def _mapped_stream(tmp_path, raw: bytes):
    wic, bm = make_wic(tmp_path, raw)
    out = tmp_path / "mapped.gz"
    bmap.stage(wic, bm, out)
    return wic, bm, out.read_bytes()


def test_writer_mapped_source_places_every_range_and_leaves_the_gaps(tmp_path):
    wic, bm, stream = _mapped_stream(tmp_path, RAW)
    p, dev = _write(tmp_path, stream, [(s, c) for s, c, _ in bm.ranges], 0, len(RAW) + BS)
    assert p.returncode == 0, p.stderr
    for s, c, _ in bm.ranges:
        assert dev[s * BS:(s + c) * BS] == RAW[s * BS:(s + c) * BS].ljust(c * BS, b"\0")
    assert dev[BS:256 * BS] == bytes([STALE]) * (255 * BS)


def test_writer_full_source_skips_the_gaps(tmp_path):
    wic, bm, _ = _mapped_stream(tmp_path, RAW)
    p, dev = _write(tmp_path, wic.read_bytes(), [(s, c) for s, c, _ in bm.ranges], len(RAW), len(RAW) + BS)
    assert p.returncode == 0, p.stderr
    for s, c, _ in bm.ranges:
        assert dev[s * BS:(s + c) * BS] == RAW[s * BS:(s + c) * BS].ljust(c * BS, b"\0")
    assert dev[BS:256 * BS] == bytes([STALE]) * (255 * BS)           # gaps are never written


def test_writer_pads_a_last_block_that_is_not_block_aligned(tmp_path):
    raw = RAW + b"\x07" * 100
    wic, bm, stream = _mapped_stream(tmp_path, raw)
    last = len(raw) // BS
    for src, size in ((wic.read_bytes(), len(raw)), (stream, 0)):
        p, dev = _write(tmp_path, src, [(s, c) for s, c, _ in bm.ranges], size, len(raw) + 2 * BS)
        assert p.returncode == 0, p.stderr
        assert dev[last * BS:(last + 1) * BS] == raw[last * BS:].ljust(BS, b"\0")    # the stale tail is zeroed


@pytest.mark.parametrize("full", [False, True])
def test_writer_refuses_a_truncated_stream(tmp_path, full):
    wic, bm, stream = _mapped_stream(tmp_path, RAW)
    src = wic.read_bytes() if full else stream
    p, _ = _write(tmp_path, src[:len(src) // 2], [(s, c) for s, c, _ in bm.ranges],
                  len(RAW) if full else 0, len(RAW) + BS)
    assert p.returncode == 1 and "bmap_writer:" in p.stderr


def test_writer_refuses_a_gzip_missing_only_its_trailer(tmp_path):
    wic, bm, stream = _mapped_stream(tmp_path, RAW)
    p, _ = _write(tmp_path, stream[:-8], [(s, c) for s, c, _ in bm.ranges], 0, len(RAW) + BS)
    assert p.returncode == 1 and "truncated" in p.stderr


def test_writer_refuses_a_stream_shorter_than_its_ranges(tmp_path):
    p, _ = _write(tmp_path, gzip.compress(b"\x01" * BS), [(0, 2)], 0, 4 * BS)
    assert p.returncode == 1 and "stream ends inside" in p.stderr


def test_writer_reports_an_io_error(tmp_path):
    p, _ = _write(tmp_path, gzip.compress(b"\x01" * BS), [(0, 1)], 0, BS)
    with open(tmp_path / "in.gz", "rb") as f:
        q = subprocess.run([sys.executable, str(bmap_writer.__file__), str(tmp_path / "no" / "dev"), str(BS),
                            str(tmp_path / "ranges"), "0"], stdin=f, capture_output=True, text=True, encoding="utf-8")
    assert p.returncode == 0 and q.returncode == 1 and "bmap_writer:" in q.stderr


def test_writer_skips_a_large_zero_gap_without_buffering_it(tmp_path):
    """A zero gap inflates ~1000:1; the writer must stay bounded and still land the tail range."""
    gap_blocks = (256 << 20) // BS                      # 256 MiB of zeros between two blocks
    head, tail = bytes([7]) * BS, bytes([9]) * BS
    co = zlib.compressobj(6, zlib.DEFLATED, 16 + zlib.MAX_WBITS)
    stream = co.compress(head)
    zero = bytes(1 << 20)
    for _ in range(256):
        stream += co.compress(zero)
    stream += co.compress(tail) + co.flush()
    size = (gap_blocks + 2) * BS
    dev, rng, gz = tmp_path / "dev.img", tmp_path / "ranges", tmp_path / "in.gz"
    with open(dev, "wb") as f:
        f.truncate(size)
    rng.write_text("0 1" + chr(10) + f"{gap_blocks + 1} 1" + chr(10), encoding="ascii")
    gz.write_bytes(stream)
    code = ("import resource, runpy, sys; sys.argv = sys.argv[1:]; "
            "resource.setrlimit(resource.RLIMIT_AS, (200 << 20, 200 << 20)); "
            "runpy.run_path(sys.argv[0], run_name='__main__')")
    try:
        import resource  # noqa: F401  (POSIX only: the address-space cap proves the bound)
        argv = [sys.executable, "-c", code]
    except ImportError:
        argv = [sys.executable]
    with open(gz, "rb") as f:
        p = subprocess.run(argv + [str(bmap_writer.__file__), str(dev), str(BS), str(rng), str(size)],
                           stdin=f, capture_output=True, text=True, encoding="utf-8")
    assert p.returncode == 0, p.stderr
    with open(dev, "rb") as f:
        assert f.read(BS) == head
        f.seek((gap_blocks + 1) * BS)
        assert f.read(BS) == tail


def test_writer_refuses_data_after_the_gzip_stream(tmp_path):
    wic, bm, stream = _mapped_stream(tmp_path, RAW)
    p, _ = _write(tmp_path, stream + b"junk", [(s, c) for s, c, _ in bm.ranges], 0, len(RAW) + BS)
    assert p.returncode == 1 and "after the end" in p.stderr
