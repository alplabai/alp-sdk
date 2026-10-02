"""The provisioning SD's payload store: hash-verified local reads, wire fallback, prepare-sd."""

from __future__ import annotations

import gzip
import hashlib
import json
import re
import shlex
import shutil
import struct
import subprocess
from pathlib import Path

import pytest
from provision import bmap, dxm1, prepare_sd, steps
from provision import linux_target as lt
from provision import payload_store as ps
from provision.store_swd_probe import StoreSwdProbe

from .test_provision_bmap import BS, RAW, STALE, BmapBoard, with_bmap
from .test_provision_steps import FIP, WritableBoard, _bench, _bundle, _ctx

OLD = b"\xaa" * 40


def _sha(ctx) -> str:
    return hashlib.sha256(json.dumps(ctx.bundle, sort_keys=True).encode()).hexdigest()


class Store:
    """The board commands the store issues; files live in self.files like everything else."""

    def _init_store(self, partition=True):
        self.partition = partition
        self.puts: list[str] = []
        self.cached: list[str] = []

    def put(self, local, remote):
        self.puts.append(remote)
        super().put(local, remote)

    def _answer(self, cmd):
        if cmd.startswith("findfs LABEL=alp-payload"):
            return (0, "/dev/mmcblk1p3\n") if self.partition else (1, "")
        if cmd.startswith("mkdir -p /mnt/alp-payload"):
            if "&& rm -f " in cmd:
                for p in shlex.split(cmd.split("&& rm -f ", 1)[1]):
                    self.files.pop(p, None)
            return 0, ""
        if m := re.match(r"test -f (/mnt/alp-payload\S+)$", cmd):
            return (0 if m[1] in self.files else 1), ""
        if m := re.match(r"sha256sum < (\S+)$", cmd):
            if m[1] not in self.files:
                return 1, ""
            return 0, hashlib.sha256(bytes(self.files[m[1]])).hexdigest() + "  -\n"
        if m := re.match(r"cp -f (\S+) (\S+)$", cmd):
            self.files[m[2]] = bytes(self.files[m[1]])
            return 0, ""
        if m := re.match(r"mv (\S+) (\S+) && sync$", cmd):
            self.files[m[2]] = self.files.pop(m[1])
            self.cached.append(m[2])
            return 0, ""
        if cmd.startswith("rm -f /mnt/alp-payload"):
            for p in shlex.split(cmd[6:]):
                self.files.pop(p, None)
            return 0, ""
        if m := re.match(r"gunzip -c (\S+) \| dd of=(\S+) bs=4M && sync$", cmd):
            self.files[m[2]] = bytearray(gzip.decompress(self.files[m[1]]))
            return 0, ""
        if m := re.match(r"gunzip -c (\S+) \| \{ pos=0;", cmd):
            dev = self.files[re.search(r"dd of=(/dev/mmcblk\d) bs=", cmd)[1]]
            bs = int(re.search(r"bs=(\d+) seek", cmd)[1])
            data, pos = gzip.decompress(self.files[m[1]]), 0
            for s, c in self._ranges():
                assert s >= pos
                dev[s * bs:(s + c) * bs] = data[s * bs:(s + c) * bs]   # the shell discards the gap
                pos = s + c
                self.written += c * bs
            return 0, ""
        return super()._answer(cmd)


class StoreBoard(Store, WritableBoard):
    def __init__(self, partition=True, **kw):
        super().__init__(**kw)
        self._init_store(partition)


class StoreBmapBoard(Store, BmapBoard):
    def __init__(self, partition=True, **kw):
        super().__init__(**kw)
        self._init_store(partition)


def _put_store(board, ctx, role, data=None):
    p = ctx.artefact(role)
    path = f"{ps.MOUNT}/{_sha(ctx)}/{p.name}"
    board.files[path] = p.read_bytes() if data is None else data
    return path


def _ctx_on(tmp_path, board, **kw):
    return _ctx(tmp_path, bench=_bench(), linux=board, execute=True, payload_store_on=True, **kw)


def _xspi(tmp_path, board):
    ctx = _ctx_on(tmp_path, board)
    return ctx, steps.run_steps(ctx, only=["write_xspi"])[-1]


def test_store_hit_with_good_hash_is_used_and_nothing_is_pushed(tmp_path):
    board = StoreBoard(xspi_bl2=b"\x00" * 16, xspi_fip=OLD)
    ctx = _ctx_on(tmp_path, board)
    _put_store(board, ctx, "bl2")
    _put_store(board, ctx, "fip")
    r = steps.run_steps(ctx, only=["write_xspi"])[-1]
    assert r.status == "done", r.detail
    assert r.evidence["payload_source"] == "sd-store"
    assert board.puts == []
    assert any(c.startswith("cp -f /mnt/alp-payload/") for c in board.commands)
    assert board.files["/dev/mtd1"][:len(FIP)] == FIP


def test_store_hit_with_bad_hash_falls_back_to_push_and_recaches(tmp_path):
    board = StoreBoard(xspi_bl2=b"\x00" * 16, xspi_fip=OLD)
    ctx = _ctx_on(tmp_path, board)
    _put_store(board, ctx, "bl2")
    bad = _put_store(board, ctx, "fip", data=b"corrupted on the SD")
    r = steps.run_steps(ctx, only=["write_xspi"])[-1]
    assert r.status == "done", r.detail
    assert r.evidence["payload_source"] == "pushed"
    assert board.puts == [bad + ".part"]
    assert board.files[bad] == FIP                      # re-cached, now good
    assert board.files["/dev/mtd1"][:len(FIP)] == FIP   # what was flashed is the bundle's, not the SD's


def test_store_miss_pushes_and_caches_for_the_next_unit(tmp_path):
    board = StoreBoard(xspi_bl2=b"\x00" * 16, xspi_fip=OLD)
    ctx, r = _xspi(tmp_path, board)
    assert r.status == "done" and r.evidence["payload_source"] == "pushed"
    assert len(board.puts) == 2 and len(board.cached) == 2


def test_no_payload_partition_pushes_to_tmp_and_says_why(tmp_path):
    board = StoreBoard(partition=False, xspi_bl2=b"\x00" * 16, xspi_fip=OLD)
    _, r = _xspi(tmp_path, board)
    assert r.status == "done" and r.evidence["payload_source"] == "pushed"
    assert "no alp-payload partition" in r.evidence["payload_store_note"]
    assert all(p.startswith("/tmp/") for p in board.puts)


def test_emmc_root_never_opens_the_store(tmp_path):
    board = StoreBoard(root="mmcblk0p2", xspi_bl2=b"\x00" * 16, xspi_fip=OLD)
    _, r = _xspi(tmp_path, board)
    assert r.evidence["payload_source"] == "pushed" and "eMMC" in r.evidence["payload_store_note"]
    assert not any("findfs" in c or "mount " in c for c in board.commands)


def test_partition_on_another_disk_is_refused(tmp_path):
    class T:
        def run(self, cmd, **kw):
            return lt.CmdResult(0, "/dev/sda1\n" if "findfs" in cmd else "", "")
    s = ps.open_store(T(), "x", [], "/dev/mmcblk1p2", "/dev/mmcblk0")
    assert s.dir is None and "not on the boot SD" in s.off


def test_host_file_that_differs_from_the_signed_bundle_is_refused(tmp_path):
    board = StoreBoard()
    ctx = _ctx_on(tmp_path, board)
    store = ctx.open_payload_store(board)
    ctx.artefact("fip").write_bytes(b"tampered after signing")
    with pytest.raises(lt.BenchError, match="differs from the signed bundle"):
        store.fetch(ctx.artefact("fip"))


def test_plan_never_mounts(tmp_path):
    board = StoreBoard(xspi_bl2=b"\x00" * 16, xspi_fip=OLD)
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, payload_store_on=True)   # execute=False
    steps.run_steps(ctx, only=["write_xspi"])
    assert not any("findfs" in c or "mount " in c for c in board.commands)


# --- GD32 over the console -------------------------------------------------------------------------

class SwdBoard(StoreBoard):
    host = "console"

    def _answer(self, cmd):
        if cmd.startswith(("cd /tmp/v2n-swd && python3", "mkdir -p /tmp/v2n-swd")):
            return 0, ""
        return super()._answer(cmd)


def _swd(tmp_path, board, populate):
    ctx = _ctx_on(tmp_path, board)
    tools = tmp_path / "tools"
    tools.mkdir()
    (tools / "swd_bb.py").write_bytes(b"print(1)")
    img = tmp_path / "slot-a.bin"
    img.write_bytes(b"\x07" * 4096)
    d = f"{ps.MOUNT}/{_sha(ctx)}"
    if populate:
        board.files[f"{d}/swd_bb.py"] = b"print(1)"
        board.files[f"{d}/slot-a.bin"] = img.read_bytes()
    store = ctx.open_payload_store(board)
    return StoreSwdProbe(board, tools, ("swd_bb.py",), store), store, img


def test_gd32_images_come_from_the_store_without_a_console_transfer(tmp_path):
    board = SwdBoard()
    probe, store, img = _swd(tmp_path, board, populate=True)
    probe.loadbin(img, 0x08010000)
    assert board.puts == []                    # a console put() is the 13-minute base64 transfer
    assert board.files["/tmp/v2n-swd/img.bin"] == img.read_bytes()
    assert any("gd32_swd_flash.py write 0x8010000" in c for c in board.commands)
    assert store.summary() == "sd-store"


def test_gd32_image_missing_from_the_store_goes_over_the_console_and_is_cached(tmp_path):
    board = SwdBoard()
    probe, store, img = _swd(tmp_path, board, populate=False)
    probe.loadbin(img, 0x08010000)
    assert any(p.endswith("/slot-a.bin.part") for p in board.puts)
    assert any(p.endswith("/slot-a.bin") for p in board.cached)
    assert store.summary() == "pushed"


# --- DX-M1 payload ---------------------------------------------------------------------------------

def test_dxm1_push_copies_from_the_store_and_still_checks_the_md5(tmp_path):
    board = StoreBoard()
    ctx = _ctx_on(tmp_path, board)
    fip = _put_store(board, ctx, "fip")
    store = ctx.open_payload_store(board)
    assert dxm1.push(board, ctx.artefact("fip"), "/tmp/dx_fw.bin", store) == hashlib.md5(FIP).hexdigest()
    assert board.puts == [] and store.summary() == "sd-store" and fip in board.files


# --- rootfs ----------------------------------------------------------------------------------------

def _rootfs(tmp_path, populate=True):
    board = StoreBmapBoard()
    ctx = _ctx_on(tmp_path, board, bundle=with_bmap(tmp_path))
    if populate:
        _put_store(board, ctx, "system_image")
    return board, ctx


def test_rootfs_bmap_runs_from_the_stored_wic_gz(tmp_path):
    board, ctx = _rootfs(tmp_path)
    r = steps.run_steps(ctx, only=["write_rootfs"])[-1]
    assert r.status == "done", r.detail
    assert r.evidence["payload_source"] == "sd-store"
    assert set(board.puts) == {lt.RANGES_PATH}                         # only the small range list
    bm = bmap.parse(ctx.artefact("system_image_bmap"))
    assert board.written == bm.mapped_bytes
    assert r.evidence["rootfs_bytes_written"] == str(bm.mapped_bytes)
    assert board.files["/dev/mmcblk0"][BS:256 * BS] == bytes([STALE]) * (255 * BS)


def test_rootfs_bad_stored_wic_is_replaced_by_a_push(tmp_path):
    board, ctx = _rootfs(tmp_path, populate=False)
    _put_store(board, ctx, "system_image", data=gzip.compress(b"not the bundle's wic"))
    r = steps.run_steps(ctx, only=["write_rootfs"])[-1]
    assert r.status == "done", r.detail
    assert r.evidence["payload_source"] == "pushed"
    assert any(p.endswith("img.wic.gz.part") for p in board.puts)


def test_rootfs_full_image_runs_from_the_stored_wic_gz(tmp_path):
    board = StoreBmapBoard()   # a bundle without a bmap: the full-image path
    ctx = _ctx_on(tmp_path, board)
    _put_store(board, ctx, "system_image")
    r = steps.run_steps(ctx, only=["write_rootfs"])[-1]
    assert r.status == "done", (r.status, r.detail)
    assert r.evidence["payload_source"] == "sd-store"
    assert any(re.match(r"gunzip -c /mnt/alp-payload/\S+ \| dd of=", c) for c in board.commands)
    assert board.puts == []


@pytest.mark.skipif(not (shutil.which("sh") and shutil.which("dd") and shutil.which("gzip")),
                    reason="needs a POSIX sh with dd and gzip")
def test_local_gap_skipping_loop_in_a_real_shell(tmp_path, monkeypatch):
    """The stored-wic write streams the WHOLE image and must discard each gap between ranges;
    run the generated command under a real sh/dd (the fakes above only model it)."""
    bdir, _ = with_bmap(tmp_path)
    wic = bdir / "artifacts" / "img.wic.gz"
    bm = bmap.parse(bdir / "artifacts" / "img.wic.bmap")
    work = tmp_path / "w"
    work.mkdir()
    shutil.copy(wic, work / "payload.gz")
    (work / "dev.img").write_bytes(bytes([STALE]) * (len(RAW) + BS))
    monkeypatch.setattr(lt, "RANGES_PATH", "ranges.txt")

    class T:
        def run(self, cmd, timeout=60.0, check=True, stdin_path=None):
            if cmd.startswith("blockdev"):
                return lt.CmdResult(0, "", "")
            p = subprocess.run(["sh", "-c", cmd], cwd=work, capture_output=True, text=True, timeout=120)
            if check and p.returncode:
                raise lt.BenchError(f"{cmd}: {p.stderr}")
            return lt.CmdResult(p.returncode, p.stdout, p.stderr)

        def put(self, local, remote):
            shutil.copy(local, work / remote)

    class S:
        def fetch(self, local):
            return "payload.gz"

    ev = lt.rootfs_write_verify_mapped(T(), "dev.img", wic, bm, store=S())
    dev = (work / "dev.img").read_bytes()
    for s, c, _ in bm.ranges:
        assert dev[s * BS:(s + c) * BS] == RAW[s * BS:(s + c) * BS].ljust(c * BS, b"\0")
    assert dev[BS:256 * BS] == bytes([STALE]) * (255 * BS)
    assert ev["rootfs_bytes_written"] == str(bm.mapped_bytes)


# --- prepare-sd ------------------------------------------------------------------------------------

def _prep(tmp_path, runner=None):
    bdir, _ = _bundle(tmp_path)
    fw = tmp_path / "fw"
    fw.mkdir()
    (fw / "slot-a.bin").write_bytes(b"\x01" * 64)
    (fw / "VERSION").write_text("0.2.7\n", encoding="utf-8")
    dev = tmp_path / "sd.img"
    calls = []

    def fake_mke2fs(argv, **kw):
        d = Path(argv[argv.index("-d") + 1])
        calls.append((argv, sorted(p.relative_to(d).as_posix() for p in d.rglob("*") if p.is_file())))
        return subprocess.CompletedProcess(argv, 0, "", "")

    msg = prepare_sd.prepare(bdir, dev, fw, runner=runner or fake_mke2fs)
    return bdir, dev, calls, msg


def test_prepare_sd_writes_wic_partition_table_and_store(tmp_path):
    bdir, dev, calls, msg = _prep(tmp_path)
    raw = gzip.decompress((bdir / "artifacts" / "img.wic.gz").read_bytes())
    img = dev.read_bytes()
    assert img[:446] == raw[:446] and img[512:len(raw)] == raw[512:]       # the wic, byte for byte
    start, sectors = next(struct.unpack_from("<II", img, 446 + 16 * i + 8)
                          for i in range(4) if img[446 + 16 * i + 4] == 0x83 and
                          struct.unpack_from("<I", img, 446 + 16 * i + 8)[0] * 512 >= len(raw))
    assert start % 2048 == 0 and len(img) >= (start + sectors) * 512
    argv, staged = calls[0]
    assert "alp-payload" in argv and f"offset={start * 512}" in argv
    sha = hashlib.sha256((bdir / "bundle.json").read_bytes()).hexdigest()
    assert staged == sorted(f"{sha}/{n}" for n in ("bl2_bp_spi.bin", "bl2_bp_mmc.bin", "fip.bin", "img.wic.gz",
                                                   "bundle.json", "manifest.sha256", "slot-a.bin", "VERSION"))
    assert sha in msg


def test_prepare_sd_manifest_lists_sha256_of_every_file(tmp_path):
    seen = {}

    def runner(argv, **kw):
        d = Path(argv[argv.index("-d") + 1])
        seen["m"] = next(d.rglob("manifest.sha256")).read_text(encoding="ascii")
        return subprocess.CompletedProcess(argv, 0, "", "")

    bdir, *_ = _prep(tmp_path, runner)
    lines = {n: h for h, n in (ln.split("  ") for ln in seen["m"].splitlines())}
    assert lines["fip.bin"] == hashlib.sha256((bdir / "artifacts" / "fip.bin").read_bytes()).hexdigest()
    assert lines["bundle.json"] == hashlib.sha256((bdir / "bundle.json").read_bytes()).hexdigest()


def test_prepare_sd_refuses_a_file_that_differs_from_the_bundle(tmp_path):
    bdir, _ = _bundle(tmp_path)
    (bdir / "artifacts" / "fip.bin").write_bytes(b"swapped")
    with pytest.raises(prepare_sd.PrepareError, match="does not match the sha256"):
        prepare_sd.prepare(bdir, tmp_path / "sd.img", None, runner=lambda *a, **k: None)
    assert not (tmp_path / "sd.img").exists()


def test_prepare_sd_reports_mke2fs_failure(tmp_path):
    with pytest.raises(prepare_sd.PrepareError, match="mke2fs failed"):
        _prep(tmp_path, lambda argv, **k: subprocess.CompletedProcess(argv, 1, "", "boom"))
