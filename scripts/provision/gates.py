# SPDX-License-Identifier: Apache-2.0
"""Offline provisioning gates for the V2N / V2N-M1 flow.

Pure checks over the release bundle, the preset and values read back from a
unit: nothing here touches hardware or the ledger. Each gate returns a
`GateResult`; the step machine (steps.py) decides what a failure stops.

Also home of the N24S128 identity-header (I2C 0x58) frame table: every 0x58
transfer the tool can emit is built by `identity_frame()`, and a frame marked
`sealed` that is not one of the table's frames cannot even be constructed.

See docs/provisioning-v2n.md.
"""

from __future__ import annotations

import enum
import gzip
import hashlib
import math
import re
import struct
import zlib
from dataclasses import dataclass
from datetime import date
from pathlib import Path


@dataclass
class GateResult:
    name: str
    ok: bool
    detail: str
    overridden: bool = False  # True when an --allow-* override turned a fail into ok


# --- artefacts: sha256 / size / role set / xSPI limits ------------------------

V2N_REQUIRED_ROLES = ("bl2", "bl2_mmc", "fip", "system_image")
XSPI_LIMIT = 16 * 1024 * 1024  # an xSPI component must be strictly smaller
# mtd1 + 0x1a0000 onward is the CM33 image; the FIP at mtd1 offset 0 must end
# before it. linux_target re-checks with the real erase-size rounding.
CM33_REGION_OFFSET = 0x1A0000
# eMMC boot1 layout: bl2_mmc from sector 1, the FIP from sector 0x300.
BL2_MMC_SECTOR = 0x1
FIP_SECTOR = 0x300
BL2_MMC_MAX = (FIP_SECTOR - BL2_MMC_SECTOR) * 512


def _sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def artefacts(bundle_dir: Path, bundle: dict) -> GateResult:
    """Every component file exists inside the bundle dir with the declared
    sha256 and size; the V2N role set is complete; xSPI images fit."""
    problems: list[str] = []
    if bundle.get("status") != "complete":
        problems.append(f"status {bundle.get('status')!r}: the V2N flow needs a 'complete' bundle")
    comps = bundle.get("components", [])
    roles = [c.get("role") for c in comps]
    for role in V2N_REQUIRED_ROLES:
        if roles.count(role) != 1:
            problems.append(f"role {role}: {roles.count(role)} component(s), need exactly 1")
    root = bundle_dir.resolve()
    for c in comps:
        role, rel = c.get("role"), c.get("file", "")
        path = (bundle_dir / rel).resolve()
        if not path.is_relative_to(root):
            problems.append(f"{role}: {rel} escapes the bundle directory")
            continue
        if not path.is_file():
            problems.append(f"{role}: {rel} missing")
            continue
        size = path.stat().st_size
        if size != c.get("size_bytes"):
            problems.append(f"{role}: size {size} != bundle.json {c.get('size_bytes')}")
        digest = _sha256(path)
        if digest != c.get("sha256"):
            problems.append(f"{role}: sha256 {digest} != bundle.json {c.get('sha256')}")
        if str(c.get("flash_target", "")).startswith("xspi:") and size >= XSPI_LIMIT:
            problems.append(f"{role}: {size} bytes does not fit xSPI (< {XSPI_LIMIT})")
        if role == "fip" and size > CM33_REGION_OFFSET:
            problems.append(f"fip: {size} bytes would reach the CM33 region at mtd1+{CM33_REGION_OFFSET:#x}")
        if role == "bl2_mmc" and size > BL2_MMC_MAX:
            problems.append(f"bl2_mmc: {size} bytes would overlap the FIP at boot1 sector {FIP_SECTOR:#x}")
    if problems:
        return GateResult("artefacts", False, "; ".join(problems))
    return GateResult("artefacts", True, f"{len(comps)} component(s) match bundle.json")


# --- SKU triangle -------------------------------------------------------------

def sku_triangle(cli_sku: str, bundle_sku: str, preset_sku: str,
                 eeprom_sku: str | None) -> GateResult:
    """--sku == bundle.sku == preset sku == EEPROM manifest SKU (exact match).
    eeprom_sku None = not read yet; that leg is skipped, not passed."""
    legs = {"cli": cli_sku, "bundle": bundle_sku, "preset": preset_sku, "eeprom": eeprom_sku}
    known = {k: v for k, v in legs.items() if v is not None}
    ok = len(set(known.values())) == 1
    detail = ", ".join(f"{k}={v}" for k, v in known.items())
    if eeprom_sku is None:
        detail += "; eeprom leg not yet known"
    return GateResult("sku_triangle", ok, detail if ok else "SKU mismatch: " + detail)


# --- DDR tier triangle ----------------------------------------------------------

def bucket_mib(mib: int | None) -> int | None:
    """U-Boot prints the usable DRAM window rounded to 0.1 GiB (a 4 GiB unit
    prints '3.9 GiB' = 3994 MiB); the tier is the nearest power of two."""
    return None if not mib else 1 << round(math.log2(mib))


def scan_tier(bl2: bytes, markers: dict) -> str:
    """DDR tier label of a BL2 image, from the PRIVATE marker json
    {"markers": [{"label": str, "hex": "<bytes>"}], "sku_tier": {...}}.
    Exactly one marker must occur in the image, else ValueError."""
    entries = markers.get("markers") if isinstance(markers, dict) else None
    if not entries:
        raise ValueError("tier markers: no 'markers' entries")
    hits = []
    for m in entries:
        pattern = bytes.fromhex(m["hex"])
        if not pattern:
            raise ValueError(f"tier marker {m.get('label')!r} has an empty pattern")
        if pattern in bl2:
            hits.append(m["label"])
    if len(hits) != 1:
        raise ValueError(f"expected exactly one DDR tier marker in BL2, matched {len(hits)}: {hits}")
    return hits[0]


def tier_triangle(bl2_images: list[bytes], markers: dict, sku: str, preset_dram_mbit: int | None,
                  bundle_tier: dict | None, uboot_dram_mib: int | None,
                  allow_mismatch: str | None) -> GateResult:
    """The SKU's tier (markers ``sku_tier[sku]``: label + Mbit) == the label
    found in every BL2 == bundle ``memory_tier.label`` (if any), and the SKU's
    Mbit == preset dram_mbit == bundle ``memory_tier.dram_mbit`` (if any) ==
    the bucketed U-Boot DRAM banner (if seen). Mbit alone cannot tell two
    tiers of the same size apart, so the label legs are what catch a
    wrong-tier BL2. allow_mismatch overrides a disagreement, never an
    unreadable BL2 tier or a SKU missing from the table."""
    name = "tier_triangle"
    if not bl2_images:
        return GateResult(name, False, "no BL2 image to scan")
    want = (markers.get("sku_tier") or {}).get(sku) if isinstance(markers, dict) else None
    if not want:
        return GateResult(name, False, f"tier markers have no sku_tier[{sku}]")
    try:
        labels = [(f"bl2[{i}]", scan_tier(img, markers)) for i, img in enumerate(bl2_images)]
    except (ValueError, KeyError, TypeError) as e:
        return GateResult(name, False, f"BL2 tier scan: {e}")
    bt = bundle_tier or {}
    labels = [(sku, want["label"]), *labels] + ([("bundle", bt["label"])] if bt.get("label") else [])
    mbits = [(sku, want.get("dram_mbit")), ("preset", preset_dram_mbit), ("bundle", bt.get("dram_mbit")),
             ("uboot", None if not uboot_dram_mib else bucket_mib(uboot_dram_mib) * 8)]
    mbits = [(k, v) for k, v in mbits if v is not None]
    detail = (", ".join(f"{k}={v}" for k, v in labels) + "; "
              + ", ".join(f"{k}={v} Mbit" for k, v in mbits))
    if len({v for _, v in labels}) == 1 and len({v for _, v in mbits}) == 1:
        return GateResult(name, True, detail)
    if allow_mismatch:
        return GateResult(name, True, f"MISMATCH {detail}; overridden: {allow_mismatch}", overridden=True)
    return GateResult(name, False, "DDR tier mismatch: " + detail)


# --- FIP content gates ----------------------------------------------------------

# Printed by U-Boot patch 0004 (uboot.py imports it from here).
RAIL_PG = "ALP: DEEPX rail 0.75V up (PG)"


def fip_rail(fip: bytes, family: str) -> GateResult:
    """A v2n-m1 FIP must carry the DEEPX 0.75 V rail bring-up (patch 0004)."""
    present = RAIL_PG.encode() in fip
    if family == "v2n-m1":
        return GateResult("fip_rail", present,
                          "rail string present" if present else f"v2n-m1 FIP lacks {RAIL_PG!r}")
    return GateResult("fip_rail", True, f"not required for family {family} (present={present})")


_FDT_RE = re.compile(rb"boot/([A-Za-z0-9_.,+-]+\.dtb)")


def fip_fdtfile(fip: bytes) -> str:
    """The dtb basename U-Boot loads (compiled in by patch 0002 as boot/<name>)."""
    names = {m.decode("ascii") for m in _FDT_RE.findall(fip)}
    if len(names) != 1:
        raise ValueError(f"expected one boot/*.dtb in the FIP, found {sorted(names)}")
    return names.pop()


# --- wic reader: MBR/GPT + ext4 /boot listing, read-only, stdlib only ----------

class _Image:
    """Random access into a .wic or .wic.gz. ponytail: a backward seek in a gzip
    stream re-decompresses from the start; the ext4 walk needs only a few. Use a
    temp-file decompress if multi-GB images make this too slow."""

    def __init__(self, path: Path) -> None:
        with open(path, "rb") as fh:
            gz = fh.read(2) == b"\x1f\x8b"
        self._f = gzip.open(path, "rb") if gz else open(path, "rb")

    def __enter__(self) -> "_Image":
        return self

    def __exit__(self, *exc) -> None:
        self._f.close()

    def read(self, offset: int, n: int) -> bytes:
        self._f.seek(offset)
        data = self._f.read(n)
        if len(data) != n:
            raise ValueError(f"image truncated: wanted {n} bytes at {offset:#x}")
        return data


def _partition_starts(img: _Image) -> list[int]:
    mbr = img.read(0, 512)
    if mbr[510:512] != b"\x55\xaa":
        raise ValueError("no MBR signature in image")
    entries = [mbr[446 + 16 * i:462 + 16 * i] for i in range(4)]
    if any(e[4] == 0xEE for e in entries):  # protective MBR -> GPT
        hdr = img.read(512, 92)
        if hdr[:8] != b"EFI PART":
            raise ValueError("protective MBR but no GPT header")
        lba, num, esz = struct.unpack_from("<QII", hdr, 72)
        table = img.read(lba * 512, num * esz)
        return [struct.unpack_from("<Q", table, i * esz + 32)[0] * 512
                for i in range(num) if table[i * esz:i * esz + 16] != bytes(16)]
    # ponytail: primary MBR entries only; logical partitions in an extended one are not walked
    return [struct.unpack_from("<I", e, 8)[0] * 512
            for e in entries if e[4] not in (0x00, 0x05, 0x0F, 0x85)]


class _NotExt4(Exception):
    pass


class _Ext4:
    def __init__(self, img: _Image, base: int) -> None:
        sb = img.read(base + 1024, 1024)
        if struct.unpack_from("<H", sb, 56)[0] != 0xEF53:
            raise _NotExt4
        self.img, self.base = img, base
        self.bs = 1024 << struct.unpack_from("<I", sb, 24)[0]
        self.first_data_block = struct.unpack_from("<I", sb, 20)[0]
        self.ipg = struct.unpack_from("<I", sb, 40)[0]
        rev = struct.unpack_from("<I", sb, 76)[0]
        self.isz = struct.unpack_from("<H", sb, 88)[0] if rev >= 1 else 128
        incompat = struct.unpack_from("<I", sb, 96)[0]
        self.dsz = (struct.unpack_from("<H", sb, 254)[0] or 64) if incompat & 0x80 else 32

    def block(self, n: int) -> bytes:
        return self.img.read(self.base + n * self.bs, self.bs)

    def inode(self, n: int) -> bytes:
        group, index = divmod(n - 1, self.ipg)
        gd = self.img.read(self.base + (self.first_data_block + 1) * self.bs + group * self.dsz, self.dsz)
        table = struct.unpack_from("<I", gd, 8)[0]
        if self.dsz >= 64:
            table |= struct.unpack_from("<I", gd, 0x28)[0] << 32
        return self.img.read(self.base + table * self.bs + index * self.isz, self.isz)

    def _extents(self, node: bytes) -> list[int]:
        magic, entries, _max, depth = struct.unpack_from("<HHHH", node, 0)
        if magic != 0xF30A:
            raise ValueError("bad ext4 extent header")
        out: list[int] = []
        for k in range(entries):
            off = 12 + 12 * k
            if depth == 0:
                _lblk, length, hi, lo = struct.unpack_from("<IHHI", node, off)
                if length > 32768:  # uninitialised extent
                    length -= 32768
                start = (hi << 32) | lo
                out.extend(range(start, start + length))
            else:
                _lblk, lo, hi = struct.unpack_from("<IIH", node, off)
                out.extend(self._extents(self.block((hi << 32) | lo)))
        return out

    def _indirect(self, ptr: int, depth: int) -> list[int]:
        if not ptr:
            return []
        ptrs = struct.unpack(f"<{self.bs // 4}I", self.block(ptr))
        if depth == 1:
            return [p for p in ptrs if p]
        return [b for p in ptrs for b in self._indirect(p, depth - 1)]

    def listdir(self, ino_no: int) -> dict[str, int]:
        ino = self.inode(ino_no)
        if struct.unpack_from("<H", ino, 0)[0] & 0xF000 != 0x4000:
            raise ValueError(f"inode {ino_no} is not a directory")
        size = struct.unpack_from("<I", ino, 4)[0] | (struct.unpack_from("<I", ino, 0x6C)[0] << 32)
        flags = struct.unpack_from("<I", ino, 0x20)[0]
        iblock = ino[0x28:0x28 + 60]
        if flags & 0x10000000:
            raise ValueError("inline-data directories are not supported")
        if flags & 0x80000:
            blocks = self._extents(iblock)
        else:
            ptrs = struct.unpack("<15I", iblock)
            blocks = [p for p in ptrs[:12] if p]
            for depth, p in ((1, ptrs[12]), (2, ptrs[13]), (3, ptrs[14])):
                blocks += self._indirect(p, depth)
        out: dict[str, int] = {}
        for b in blocks[:-(-size // self.bs)]:
            data, off = self.block(b), 0
            while off + 8 <= len(data):
                inode, rec_len, name_len, _ftype = struct.unpack_from("<IHBB", data, off)
                if rec_len < 8:
                    break
                if inode and name_len:  # inode 0 = unused / htree node / csum tail
                    out[data[off + 8:off + 8 + name_len].decode("utf-8", "replace")] = inode
                off += rec_len
        return out


def wic_boot_files(wic_gz: Path) -> set[str]:
    """Names in /boot of every ext4 partition of the (gzipped) wic. Read-only."""
    found: set[str] = set()
    ext4_seen = False
    with _Image(wic_gz) as img:
        for start in _partition_starts(img):
            try:
                fs = _Ext4(img, start)
            except _NotExt4:
                continue
            ext4_seen = True
            root = fs.listdir(2)
            if "boot" in root:
                found |= set(fs.listdir(root["boot"])) - {".", ".."}
    if not ext4_seen:
        raise ValueError("no ext4 partition in the image")
    return found


def fdt(fip: bytes, wic_gz: Path) -> GateResult:
    """The dtb U-Boot will load exists in the rootfs /boot/."""
    try:
        name = fip_fdtfile(fip)
        files = wic_boot_files(wic_gz)
    except (ValueError, OSError, EOFError, zlib.error) as e:
        return GateResult("fdt", False, str(e))
    if name in files:
        return GateResult("fdt", True, f"boot/{name} present in the wic rootfs")
    dtbs = sorted(f for f in files if f.endswith(".dtb"))
    return GateResult("fdt", False, f"FIP loads boot/{name}; wic /boot has {dtbs}")


# --- serial / mfg_date ----------------------------------------------------------

_SERIAL_RE = re.compile(r"^(\d{4})W(\d{2})-(\d{4})$")


def parse_serial(serial: str) -> tuple[int, int, int]:
    """'YYYYWww-NNNN' -> (year, iso_week, seq). ValueError on a bad serial."""
    m = _SERIAL_RE.match(serial)
    if not m:
        raise ValueError(f"serial {serial!r} is not YYYYWww-NNNN")
    year, week, seq = (int(g) for g in m.groups())
    if seq == 0:
        raise ValueError(f"serial {serial!r}: sequence starts at 0001")
    date.fromisocalendar(year, week, 1)  # ValueError for a week the year lacks
    return year, week, seq


def mfg_date_for_serial(serial: str) -> date:
    """Monday of the serial's ISO week (maintainer decision; never typed)."""
    year, week, _ = parse_serial(serial)
    return date.fromisocalendar(year, week, 1)


# --- N24S128 identity header (design section 7) ---------------------------------

EEPROM_ADDR = 0x50
IDENTITY_ADDR = 0x58
SECURE_PAGE_LEN = 64


class IdentityOp(enum.Enum):
    SECURE_PAGE_WRITE = "secure_page_write"
    SECURE_PAGE_READ = "secure_page_read"
    UNIQUE_ID_READ = "unique_id_read"
    LOCK_STATUS_READ = "lock_status_read"
    DEVICE_CONFIG_READ = "device_config_read"
    LOCK = "lock"


# op -> (pointer/selector bytes written, bytes read by repeated start).
# The selector (first byte) can permanently alter the part: this table is the
# only source of 0x58 frames. 0x06 appears only as a pointer + read-1.
IDENTITY_TABLE: dict[IdentityOp, tuple[bytes, int]] = {
    IdentityOp.SECURE_PAGE_WRITE: (b"\x00\x00", 0),  # + 64 data bytes
    IdentityOp.SECURE_PAGE_READ: (b"\x00\x00", SECURE_PAGE_LEN),
    IdentityOp.UNIQUE_ID_READ: (b"\x02\x00", 16),
    IdentityOp.LOCK_STATUS_READ: (b"\x04\x00", 1),
    IdentityOp.DEVICE_CONFIG_READ: (b"\x06\x00", 1),
    IdentityOp.LOCK: (b"\x04\x00\xff", 0),
}


def _in_identity_table(write: bytes, read_len: int) -> bool:
    for op, (prefix, rl) in IDENTITY_TABLE.items():
        if op is IdentityOp.SECURE_PAGE_WRITE:
            if read_len == 0 and len(write) == len(prefix) + SECURE_PAGE_LEN and write[:2] == prefix:
                return True
        elif write == prefix and read_len == rl:
            return True
    return False


@dataclass(frozen=True)
class I2cFrame:
    addr: int
    write: bytes
    read_len: int
    sealed: bool = False  # only identity_frame() sets True

    def __post_init__(self) -> None:
        # A sealed frame must be a table frame, so `sealed` cannot be forged
        # onto an arbitrary 0x58 write.
        if self.sealed and not (self.addr == IDENTITY_ADDR
                                and _in_identity_table(self.write, self.read_len)):
            raise ValueError("sealed I2C frame is not in the N24S128 identity table")


def identity_frame(op: IdentityOp, payload: bytes = b"") -> I2cFrame:
    prefix, read_len = IDENTITY_TABLE[op]
    if op is IdentityOp.SECURE_PAGE_WRITE:
        if len(payload) != SECURE_PAGE_LEN:
            raise ValueError(f"secure page write needs {SECURE_PAGE_LEN} bytes, got {len(payload)}")
    elif payload:
        raise ValueError(f"{op.value} takes no payload")
    write = prefix + bytes(payload)
    # Explicit raise, not `assert`: must survive python -O.
    if write[0] == 0x06 and (len(write) > 2 or read_len == 0):
        raise AssertionError("selector 0x06 frame carrying data or without a read")
    return I2cFrame(IDENTITY_ADDR, write, read_len, sealed=True)
