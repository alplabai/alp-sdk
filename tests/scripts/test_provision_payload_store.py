"""The provisioning SD's payload store: hash-verified local reads, wire fallback, free-space check."""

from __future__ import annotations

import gzip
import hashlib
import json
import re
import shlex
import shutil

import pytest
from provision import bmap, dxm1, steps
from provision import linux_target as lt
from provision import payload_store as ps
from provision.store_swd_probe import StoreSwdProbe

from .test_provision_bmap import BS, RAW, STALE, BmapBoard, RealShell, needs_sh, with_bmap
from .test_provision_steps import FIP, WritableBoard, _bench, _bundle, _ctx

OLD = b"\xaa" * 40


def _sha(ctx) -> str:
    return hashlib.sha256(json.dumps(ctx.bundle, sort_keys=True).encode()).hexdigest()


class Store:
    """The board commands the store issues; files live in self.files like everything else."""

    def _init_store(self, free_kib=10_000_000):
        self.puts: list[str] = []
        self.cached: list[str] = []
        self.free_kib = free_kib
        self.cmp_missing = False

    def put(self, local, remote):
        self.puts.append(remote)
        super().put(local, remote)

    def _answer(self, cmd):
        if cmd.startswith("df -k "):
            return 0, ("Filesystem 1K-blocks Used Available Use% Mounted on\n"
                       f"/dev/mmcblk1p2 6815744 600000 {self.free_kib} 9% /\n")
        if cmd.startswith(f"mkdir -p {ps.STORE_ROOT}"):
            if "&& rm -f " in cmd:
                for p in shlex.split(cmd.split("&& rm -f ", 1)[1]):
                    self.files.pop(p, None)
            return 0, ""
        if m := re.match(rf"test -f ({ps.STORE_ROOT}\S+)$", cmd):
            return (0 if m[1] in self.files else 1), ""
        if m := re.match(r"sha256sum < (\S+)$", cmd):
            if m[1] not in self.files:
                return 1, ""
            return 0, hashlib.sha256(bytes(self.files[m[1]])).hexdigest() + "  -\n"
        if m := re.match(r"cp -f (\S+) (\S+) && cmp -s \S+ \S+$", cmd):
            self.files[m[2]] = bytes(self.files[m[1]])
            return (127, "cmp: not found") if self.cmp_missing else (0, "")
        if m := re.match(r"mv (\S+) (\S+) && sync$", cmd):
            self.files[m[2]] = self.files.pop(m[1])
            self.cached.append(m[2])
            return 0, ""
        if cmd.startswith(f"rm -f {ps.STORE_ROOT}"):
            for p in shlex.split(cmd[6:]):
                self.files.pop(p, None)
            return 0, ""
        if m := re.match(r"gunzip -c (\S+) \| dd of=(\S+) bs=4M && sync$", cmd):
            self.files[m[2]] = bytearray(gzip.decompress(self.files[m[1]]))
            return 0, ""
        return super()._answer(cmd)


class StoreBoard(Store, WritableBoard):
    def __init__(self, free_kib=10_000_000, **kw):
        super().__init__(**kw)
        self._init_store(free_kib)


class StoreBmapBoard(Store, BmapBoard):
    def __init__(self, free_kib=10_000_000, **kw):
        super().__init__(**kw)
        self._init_store(free_kib)


def _put_store(board, ctx, role, data=None):
    p = ctx.artefact(role)
    path = f"{ps.STORE_ROOT}/{_sha(ctx)}/{p.name}"
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
    assert any(c.startswith("cp -f /var/lib/alp-payload/") for c in board.commands)
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


def test_too_little_free_space_pushes_to_tmp_and_says_why(tmp_path):
    board = StoreBoard(free_kib=50 * 1024, xspi_bl2=b"\x00" * 16, xspi_fip=OLD)   # < size + 20% + 64 MiB
    _, r = _xspi(tmp_path, board)
    assert r.status == "done" and r.evidence["payload_source"] == "pushed"
    assert "not cached" in r.evidence["payload_store_note"] and "need" in r.evidence["payload_store_note"]
    assert all(p.startswith("/tmp/") for p in board.puts) and board.cached == []


def test_free_space_check_needs_size_plus_20_percent_plus_64_mib(tmp_path):
    board = StoreBoard()
    ctx = _ctx_on(tmp_path, board)
    store = ctx.open_payload_store(board)
    f = tmp_path / "big.bin"
    with open(f, "wb") as fh:
        fh.truncate(100 << 20)                                  # 100 MiB: needs 100 + 20 + 64 = 184 MiB
    dest = "/var/lib/alp-payload/x/big.bin"
    board.free_kib = (184 << 10) - 1
    assert not store._cache(f, dest, dest, "0")
    assert board.puts == []                                     # refused before any transfer
    board.free_kib = 184 << 10
    assert not store._cache(f, dest, dest, "0")                 # space is enough; the fake hash "0" fails
    assert board.puts == [dest + ".part"]


def test_emmc_root_never_opens_the_store(tmp_path):
    board = StoreBoard(root="mmcblk0p2", xspi_bl2=b"\x00" * 16, xspi_fip=OLD)
    _, r = _xspi(tmp_path, board)
    assert r.evidence["payload_source"] == "pushed" and "eMMC" in r.evidence["payload_store_note"]
    assert not any(c.startswith(("df -k", "mkdir -p /var/lib")) for c in board.commands)


def test_the_store_is_a_directory_on_the_root_filesystem(tmp_path):
    board = StoreBoard(xspi_bl2=b"\x00" * 16, xspi_fip=OLD)
    ctx, r = _xspi(tmp_path, board)
    assert r.status == "done", r.detail
    assert board.cached and all(p.startswith(f"/var/lib/alp-payload/{_sha(ctx)}/") for p in board.cached)
    assert not any(c.startswith(("mount -o", "umount")) or "findfs" in c or "remount" in c for c in board.commands)      # no partition, no mount
    assert any(c.endswith("&& sync") for c in board.commands)


def test_host_file_that_differs_from_the_signed_bundle_is_refused(tmp_path):
    board = StoreBoard()
    ctx = _ctx_on(tmp_path, board)
    store = ctx.open_payload_store(board)
    ctx.artefact("fip").write_bytes(b"tampered after signing")
    with pytest.raises(lt.BenchError, match="differs from the signed bundle"):
        store.fetch(ctx.artefact("fip"))


def test_plan_never_touches_the_store(tmp_path):
    board = StoreBoard(xspi_bl2=b"\x00" * 16, xspi_fip=OLD)
    ctx = _ctx(tmp_path, bench=_bench(), linux=board, payload_store_on=True)   # execute=False
    steps.run_steps(ctx, only=["write_xspi"])
    assert not any(c.startswith(("df -k", "mkdir -p /var/lib")) for c in board.commands)


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
    d = f"{ps.STORE_ROOT}/{_sha(ctx)}"
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
    assert set(board.puts) == {lt.RANGES_PATH, lt.WRITER_PATH}         # range list + writer, not the wic
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
    assert any(re.match(r"gunzip -c /var/lib/alp-payload/\S+ \| dd of=", c) for c in board.commands)
    assert board.puts == []


@needs_sh
def test_stored_wic_write_skips_the_gaps_in_a_real_shell(tmp_path, monkeypatch):
    """The stored-wic write streams the WHOLE image and must skip each gap between ranges; run
    the real writer through a real sh (the fakes above only interpret it)."""
    bdir, _ = with_bmap(tmp_path)
    wic = bdir / "artifacts" / "img.wic.gz"
    bm = bmap.parse(bdir / "artifacts" / "img.wic.bmap")
    sh = RealShell(tmp_path / "w", monkeypatch)
    shutil.copy(wic, sh.work / "payload.gz")
    (sh.work / "dev.img").write_bytes(bytes([STALE]) * (len(RAW) + BS))

    class S:
        def fetch(self, local):
            return "payload.gz"

    ev = lt.rootfs_write_verify_mapped(sh, "dev.img", wic, bm, store=S())
    dev = (sh.work / "dev.img").read_bytes()
    for s, c, _ in bm.ranges:
        assert dev[s * BS:(s + c) * BS] == RAW[s * BS:(s + c) * BS].ljust(c * BS, b"\0")
    assert dev[BS:256 * BS] == bytes([STALE]) * (255 * BS)
    assert ev["rootfs_bytes_written"] == str(bm.mapped_bytes)


def test_a_store_hit_writes_nothing_and_checks_no_space(tmp_path):
    board = StoreBoard(xspi_bl2=b"\x00" * 16, xspi_fip=OLD)
    ctx = _ctx_on(tmp_path, board)
    _put_store(board, ctx, "bl2")
    _put_store(board, ctx, "fip")
    steps.run_steps(ctx, only=["write_xspi"])
    assert not any(c.startswith(("df -k", "mkdir -p")) for c in board.commands)


def test_stage_compares_md5_when_the_board_has_no_cmp(tmp_path):
    board = StoreBoard(xspi_bl2=b"\x00" * 16, xspi_fip=OLD)
    board.cmp_missing = True
    ctx = _ctx_on(tmp_path, board)
    _put_store(board, ctx, "bl2")
    _put_store(board, ctx, "fip")
    r = steps.run_steps(ctx, only=["write_xspi"])[-1]
    assert r.status == "done", r.detail
    assert board.files["/dev/mtd1"][:len(FIP)] == FIP


def test_stage_fails_when_the_copy_differs(tmp_path):
    board = StoreBoard()
    board.files["/var/lib/alp-payload/x/f"] = b"abc"
    board.run = lambda cmd, timeout=60.0, check=True, stdin_path=None: lt.CmdResult(1, "differ", "")
    local = tmp_path / "f"
    local.write_bytes(b"abc")

    class S:
        def fetch(self, loc):
            return "/var/lib/alp-payload/x/f"
    with pytest.raises(lt.BenchError, match="failed or differs"):
        ps.stage(board, S(), local, "/tmp/f")


def test_failed_push_cleanup_also_removes_the_console_chunk_files(tmp_path):
    board = StoreBoard()
    ctx = _ctx_on(tmp_path, board)
    store = ctx.open_payload_store(board)
    board.put = lambda local, remote: (_ for _ in ()).throw(lt.BenchError("garbled"))
    store.fetch(ctx.artefact("fip"))
    cleanup = [c for c in board.commands if c.startswith("rm -f /var/lib/alp-payload")]
    assert cleanup and all(".part.p*" in c and ".part.b64" in c for c in cleanup)
    assert any(".part.p*" in c for c in board.commands if c.startswith("mkdir -p /var/lib/alp-payload"))
