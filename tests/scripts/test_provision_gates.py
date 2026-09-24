"""Unit tests for scripts/provision/gates.py (all synthetic, no hardware)."""

from __future__ import annotations

import gzip
import hashlib
import struct
from datetime import date

import pytest

from provision import gates

BS = 1024
IPG = 16
ISZ = 256
ITABLE = 3  # inode table blocks 3..6 (16 * 256 B)


# --- synthetic ext4 -------------------------------------------------------------

def _dir_block(entries: list[tuple[int, str]], csum_tail: bool = True) -> bytes:
    """Linear ext4 dir block; last real entry spans to the (optional) csum tail."""
    out = b""
    end = BS - 12 if csum_tail else BS
    for i, (ino, name) in enumerate(entries):
        raw = name.encode()
        rec = (8 + len(raw) + 3) & ~3
        if i == len(entries) - 1:
            rec = end - len(out)
        out += struct.pack("<IHBB", ino, rec, len(raw), 2) + raw
        out += bytes(rec - 8 - len(raw))
    if csum_tail:  # metadata_csum tail: inode 0, rec_len 12, name_len 0, type 0xde
        out += struct.pack("<IHBB", 0, 12, 0, 0xDE) + bytes(4)
    return out


def _extent_iblock(depth: int, entries: list[tuple]) -> bytes:
    hdr = struct.pack("<HHHHI", 0xF30A, len(entries), 4, depth, 0)
    body = b""
    for e in entries:
        body += struct.pack("<IHHI", *e) if depth == 0 else struct.pack("<IIHH", *e, 0)
    return (hdr + body).ljust(60, b"\0")


def _inode(iblock: bytes, size: int, extents: bool) -> bytes:
    ino = bytearray(ISZ)
    struct.pack_into("<H", ino, 0, 0x41ED)
    struct.pack_into("<I", ino, 4, size)
    struct.pack_into("<I", ino, 0x20, 0x80000 if extents else 0)
    ino[0x28:0x28 + 60] = iblock
    return bytes(ino)


def _ext4(boot_names: list[str], *, extents: bool = True, desc64: bool = False,
          with_boot: bool = True) -> bytes:
    nblocks = 16
    fs = bytearray(nblocks * BS)
    sb = bytearray(1024)
    struct.pack_into("<I", sb, 20, 1)          # first data block (1 KiB blocks)
    struct.pack_into("<I", sb, 24, 0)          # log block size -> 1024
    struct.pack_into("<I", sb, 40, IPG)
    struct.pack_into("<H", sb, 56, 0xEF53)
    struct.pack_into("<I", sb, 76, 1)
    struct.pack_into("<H", sb, 88, ISZ)
    struct.pack_into("<I", sb, 96, 0x40 | (0x80 if desc64 else 0))
    struct.pack_into("<H", sb, 254, 64 if desc64 else 0)
    fs[1024:2048] = sb
    struct.pack_into("<I", fs, 2 * BS + 8, ITABLE)  # group 0 inode table
    root_entries = [(2, "."), (2, ".."), (11, "lost+found")]
    if with_boot:
        root_entries.append((12, "boot"))
    fs[7 * BS:8 * BS] = _dir_block(root_entries)
    # boot dir: two blocks (9, 10) to exercise multi-block listing
    half = len(boot_names) // 2
    fs[9 * BS:10 * BS] = _dir_block([(12, "."), (2, "..")] + [(20 + i, n) for i, n in enumerate(boot_names[:half])])
    fs[10 * BS:11 * BS] = _dir_block([(40 + i, n) for i, n in enumerate(boot_names[half:])] or [(0, "")])
    if extents:
        root_ib = _extent_iblock(0, [(0, 1, 0, 7)])
        fs[8 * BS:9 * BS] = _extent_iblock(0, [(0, 2, 0, 9)]).ljust(BS, b"\0")  # leaf node block
        boot_ib = _extent_iblock(1, [(0, 8, 0)])  # depth-1 index -> leaf at block 8
    else:
        root_ib = struct.pack("<15I", 7, *([0] * 14))
        struct.pack_into("<2I", fs, 8 * BS, 10, 0)  # single-indirect block 8 -> block 10
        boot_ib = struct.pack("<15I", 9, *([0] * 11), 8, 0, 0)
    for n, ib, size in ((2, root_ib, BS), (12, boot_ib, 2 * BS)):
        off = ITABLE * BS + (n - 1) * ISZ
        fs[off:off + ISZ] = _inode(ib, size, extents)
    return bytes(fs)


def _mbr(parts: list[tuple[int, int, int]]) -> bytearray:
    mbr = bytearray(512)
    for i, (ptype, lba, count) in enumerate(parts):
        struct.pack_into("<4xB3xII", mbr, 446 + 16 * i, ptype, lba, count)
    mbr[510:512] = b"\x55\xaa"
    return mbr


def _wic(fs: bytes, gpt: bool = False) -> bytes:
    start = 2048
    img = bytearray(start * 512 + len(fs))
    if gpt:
        img[0:512] = _mbr([(0xEE, 1, 0xFFFFFFFF)])
        hdr = bytearray(92)
        hdr[:8] = b"EFI PART"
        struct.pack_into("<QII", hdr, 72, 2, 4, 128)
        img[512:604] = hdr
        entry = bytearray(128)
        entry[:16] = b"\x01" * 16
        struct.pack_into("<Q", entry, 32, start)
        img[1024:1152] = entry
    else:
        # partition 1 is not ext4 (skipped), partition 2 is
        img[0:512] = _mbr([(0x0C, 64, 64), (0x83, start, len(fs) // 512)])
    img[start * 512:] = fs
    return bytes(img)


def _write_gz(tmp_path, raw: bytes, name="img.wic.gz"):
    p = tmp_path / name
    p.write_bytes(gzip.compress(raw))
    return p


FIP = b"\0" * 64 + b"ext4load mmc 0:1 0x48000000 boot/alp-test.dtb\0boot/Image\0" + b"\xff" * 32


@pytest.mark.parametrize("kw", [{}, {"extents": False}, {"desc64": True}, {"gpt": True}])
def test_wic_boot_files_variants(tmp_path, kw):
    gpt = kw.pop("gpt", False)
    names = ["Image", "alp-test.dtb", "other.dtb"]
    p = _write_gz(tmp_path, _wic(_ext4(names, **kw), gpt=gpt))
    assert gates.wic_boot_files(p) == set(names)


def test_wic_boot_files_uncompressed(tmp_path):
    p = tmp_path / "img.wic"
    p.write_bytes(_wic(_ext4(["alp-test.dtb"])))
    assert gates.wic_boot_files(p) == {"alp-test.dtb"}


def test_wic_without_ext4_raises(tmp_path):
    img = bytearray(4096)
    img[0:512] = _mbr([(0x0C, 1, 4)])
    with pytest.raises(ValueError, match="no ext4"):
        gates.wic_boot_files(_write_gz(tmp_path, bytes(img)))


def test_fdt_gate(tmp_path):
    good = _write_gz(tmp_path, _wic(_ext4(["Image", "alp-test.dtb"])), "good.wic.gz")
    bad = _write_gz(tmp_path, _wic(_ext4(["Image", "wrong.dtb"])), "bad.wic.gz")
    noboot = _write_gz(tmp_path, _wic(_ext4([], with_boot=False)), "noboot.wic.gz")
    assert gates.fdt(FIP, good).ok
    r = gates.fdt(FIP, bad)
    assert not r.ok and "wrong.dtb" in r.detail
    assert not gates.fdt(FIP, noboot).ok
    trunc = tmp_path / "trunc.wic.gz"
    trunc.write_bytes(good.read_bytes()[:200])
    assert not gates.fdt(FIP, trunc).ok


def test_fip_fdtfile():
    assert gates.fip_fdtfile(FIP) == "alp-test.dtb"
    assert gates.fip_fdtfile(FIP + b"boot/alp-test.dtb") == "alp-test.dtb"  # same name twice is fine
    with pytest.raises(ValueError):
        gates.fip_fdtfile(b"no dtb here")
    with pytest.raises(ValueError):
        gates.fip_fdtfile(FIP + b"boot/other.dtb")


# --- rail ------------------------------------------------------------------------

def test_fip_rail():
    with_rail = b"\0" + gates.RAIL_PG.encode() + b"\0"
    assert gates.fip_rail(with_rail, "v2n-m1").ok
    assert not gates.fip_rail(b"\0" * 16, "v2n-m1").ok
    assert gates.fip_rail(b"\0" * 16, "v2n").ok


def test_rail_string_matches_uboot_module():
    uboot = pytest.importorskip("provision.uboot")
    assert gates.RAIL_PG == uboot.RAIL_PG


# --- SKU / tier triangles -----------------------------------------------------------

def test_sku_triangle():
    assert gates.sku_triangle("E1M-V2N101", "E1M-V2N101", "E1M-V2N101", None).ok
    assert gates.sku_triangle("E1M-V2N101", "E1M-V2N101", "E1M-V2N101", "E1M-V2N101").ok
    r = gates.sku_triangle("E1M-V2N101", "E1M-V2N101", "E1M-V2N101", "E1M-V2M101")
    assert not r.ok and "eeprom=E1M-V2M101" in r.detail
    assert not gates.sku_triangle("E1M-V2N101", "E1M-V2N102", "E1M-V2N101", None).ok


MARKERS = {"markers": [
    {"hex": "de ad be ef", "label": "t16"},
    {"hex": "ca fe ba be", "label": "t32"},
    {"hex": "0b 0b 0b 0b", "label": "t32b"},     # a second tier of the same size
], "sku_tier": {"S32": {"label": "t32", "dram_mbit": 32768},
                "S32B": {"label": "t32b", "dram_mbit": 32768},
                "S16": {"label": "t16", "dram_mbit": 16384}}}
BL2_32 = b"\0" * 8 + bytes.fromhex("cafebabe") + b"\0" * 8
BL2_16 = b"\0" * 8 + bytes.fromhex("deadbeef")


def test_scan_tier():
    assert gates.scan_tier(BL2_32, MARKERS) == "t32"
    with pytest.raises(ValueError, match="matched 0"):
        gates.scan_tier(b"\0" * 16, MARKERS)
    with pytest.raises(ValueError, match="matched 2"):
        gates.scan_tier(BL2_32 + BL2_16, MARKERS)
    with pytest.raises(ValueError):
        gates.scan_tier(BL2_32, {"markers": []})


def test_bucket_mib():
    assert gates.bucket_mib(3994) == 4096 and gates.bucket_mib(1946) == 2048
    assert gates.bucket_mib(None) is None


def test_tier_triangle():
    tt = gates.tier_triangle
    assert tt([BL2_32, BL2_32], MARKERS, "S32", 32768, {"dram_mbit": 32768}, 3994, None).ok
    assert tt([BL2_32], MARKERS, "S32", 32768, None, None, None).ok
    r = tt([BL2_32, BL2_16], MARKERS, "S32", 32768, None, None, None)
    assert not r.ok and not r.overridden
    assert not tt([BL2_32], MARKERS, "S32", 32768, None, 1946, None).ok        # banner leg
    # same Mbit, different tier: only the label legs catch it
    r = tt([BL2_32], MARKERS, "S32B", 32768, {"dram_mbit": 32768}, 3994, None)
    assert not r.ok and "DDR tier mismatch" in r.detail and "t32b" in r.detail
    assert not tt([BL2_32], MARKERS, "S32", 32768, {"dram_mbit": 32768, "label": "t32b"}, None, None).ok
    r = tt([BL2_32], MARKERS, "S32", 16384, None, None, "bench unit, rework pending")
    assert r.ok and r.overridden and "rework pending" in r.detail
    # an unreadable tier or an unknown SKU is never overridable
    r = tt([b"\0"], MARKERS, "S32", 32768, None, None, "reason")
    assert not r.ok and not r.overridden
    assert not tt([BL2_32], MARKERS, "NOPE", 32768, None, None, "reason").ok
    assert not tt([], MARKERS, "S32", 32768, None, None, None).ok


# --- artefacts ------------------------------------------------------------------

def _bundle(tmp_path, sizes=None):
    sizes = sizes or {}
    comps = []
    for role, target in (("bl2", "xspi:mtd0"), ("bl2_mmc", "emmc:boot1"),
                         ("fip", "xspi:mtd1"), ("system_image", "emmc")):
        data = b"x" * sizes.get(role, 8)
        f = tmp_path / "artifacts" / f"{role}.bin"
        f.parent.mkdir(exist_ok=True)
        f.write_bytes(data)
        comps.append({"role": role, "file": f"artifacts/{role}.bin",
                      "sha256": hashlib.sha256(data).hexdigest(),
                      "size_bytes": len(data), "flash_target": target})
    return {"status": "complete", "components": comps}


def test_artefacts_ok(tmp_path):
    assert gates.artefacts(tmp_path, _bundle(tmp_path)).ok


def test_artefacts_hash_size_and_missing(tmp_path):
    b = _bundle(tmp_path)
    b["components"][0]["sha256"] = "0" * 64
    b["components"][1]["size_bytes"] = 9
    (tmp_path / "artifacts" / "fip.bin").unlink()
    r = gates.artefacts(tmp_path, b)
    assert not r.ok
    assert "bl2: sha256" in r.detail and "bl2_mmc: size" in r.detail and "fip: artifacts/fip.bin missing" in r.detail


def test_artefacts_roles_status_escape(tmp_path):
    b = _bundle(tmp_path)
    b["components"] = [c for c in b["components"] if c["role"] != "bl2_mmc"]
    b["status"] = "bootloader-only:image-pending-hw"
    b["components"][0]["file"] = "../outside.bin"
    r = gates.artefacts(tmp_path, b)
    assert "role bl2_mmc: 0" in r.detail and "status" in r.detail and "escapes" in r.detail


def test_artefacts_xspi_limits(tmp_path):
    r = gates.artefacts(tmp_path, _bundle(tmp_path, {"bl2": gates.XSPI_LIMIT}))
    assert not r.ok and "does not fit xSPI" in r.detail
    r = gates.artefacts(tmp_path, _bundle(tmp_path, {"fip": gates.CM33_REGION_OFFSET + 1}))
    assert not r.ok and "CM33 region" in r.detail
    assert gates.artefacts(tmp_path, _bundle(tmp_path, {"fip": gates.CM33_REGION_OFFSET})).ok
    r = gates.artefacts(tmp_path, _bundle(tmp_path, {"bl2_mmc": gates.BL2_MMC_MAX + 1}))
    assert not r.ok and "overlap the FIP" in r.detail
    assert gates.artefacts(tmp_path, _bundle(tmp_path, {"bl2_mmc": gates.BL2_MMC_MAX})).ok


# --- serial / mfg_date ----------------------------------------------------------

def test_serial_and_mfg_date():
    assert gates.parse_serial("2026W38-0001") == (2026, 38, 1)
    assert gates.mfg_date_for_serial("2026W38-0001") == date(2026, 9, 14)
    assert gates.mfg_date_for_serial("2026W01-0003") == date(2025, 12, 29)  # ISO week 1 starts in 2025
    assert gates.mfg_date_for_serial("2026W53-0001") == date(2026, 12, 28)  # 2026 has 53 ISO weeks
    for bad in ("2026w38-0001", "2026W38-1", "2026W38-0000", "2025W53-0001", "2026W00-0001", " 2026W38-0001"):
        with pytest.raises(ValueError):
            gates.parse_serial(bad)


# --- N24S128 identity frames ------------------------------------------------------

def test_identity_frames_match_table():
    Op = gates.IdentityOp
    f = gates.identity_frame(Op.SECURE_PAGE_WRITE, bytes(range(64)))
    assert (f.addr, f.write[:2], len(f.write), f.read_len, f.sealed) == (0x58, b"\x00\x00", 66, 0, True)
    assert gates.identity_frame(Op.SECURE_PAGE_READ).write == b"\x00\x00"
    assert gates.identity_frame(Op.SECURE_PAGE_READ).read_len == 64
    assert (gates.identity_frame(Op.UNIQUE_ID_READ).write, gates.identity_frame(Op.UNIQUE_ID_READ).read_len) == (b"\x02\x00", 16)
    assert (gates.identity_frame(Op.LOCK_STATUS_READ).write, gates.identity_frame(Op.LOCK_STATUS_READ).read_len) == (b"\x04\x00", 1)
    assert (gates.identity_frame(Op.DEVICE_CONFIG_READ).write, gates.identity_frame(Op.DEVICE_CONFIG_READ).read_len) == (b"\x06\x00", 1)
    assert (gates.identity_frame(Op.LOCK).write, gates.identity_frame(Op.LOCK).read_len) == (b"\x04\x00\xff", 0)


def test_no_frame_writes_selector_0x06():
    """Structural: no table entry yields a data-bearing or read-less 0x06 frame."""
    for op in gates.IdentityOp:
        payload = bytes(64) if op is gates.IdentityOp.SECURE_PAGE_WRITE else b""
        f = gates.identity_frame(op, payload)
        assert f.addr == gates.IDENTITY_ADDR and f.sealed
        if f.write[0] == 0x06:
            assert len(f.write) == 2 and f.read_len == 1


def test_identity_payload_rules():
    with pytest.raises(ValueError):
        gates.identity_frame(gates.IdentityOp.SECURE_PAGE_WRITE, bytes(63))
    with pytest.raises(ValueError):
        gates.identity_frame(gates.IdentityOp.LOCK, b"\x01")
    with pytest.raises(ValueError):
        gates.identity_frame(gates.IdentityOp.DEVICE_CONFIG_READ, b"\x01")


@pytest.mark.parametrize("write,read_len,addr", [
    (b"\x06\x00\x01", 0, 0x58),   # config write
    (b"\x06\x00", 0, 0x58),       # standalone pointer write
    (b"\x04\x00\x00", 0, 0x58),   # lock status with a non-FF byte
    (b"\x00\x00", 64, 0x50),      # right shape, wrong address
    (b"\x00\x00" + bytes(10), 0, 0x58),  # short secure-page write
])
def test_sealed_frame_cannot_be_forged(write, read_len, addr):
    with pytest.raises(ValueError):
        gates.I2cFrame(addr, write, read_len, sealed=True)
    assert not gates.I2cFrame(addr, write, read_len).sealed  # unsealed is allowed; linux_target refuses it at 0x58
