"""Linux-side helpers for V2N provisioning: SSH runner, console login,
MTD / eMMC / I2C helpers and the read-only census.

Every command goes through ``LinuxTarget.run()`` (one ``ssh`` per call), so
tests replay a scripted runner and no hardware is needed. Nothing here
decides *whether* to write: callers (``steps.py``) wrap each write helper in
``ctx.mutate()``. See docs/provisioning-v2n.md.
"""

from __future__ import annotations

import gzip
import hashlib
import re
import shlex
import subprocess
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

from provision.bench import BenchError
from provision.gates import CM33_REGION_OFFSET

if TYPE_CHECKING:
    from provision.bench import Console

EEPROM_ADDR = 0x50
IDENTITY_ADDR = 0x58
EEPROM_SIZE = 0x4000           # N24S128: 16 KiB, 2-byte word address
EEPROM_DEVICE_PAGE = 64        # hardware page; a page write must not cross it
MANIFEST_LEN = 128

# SoC SYS block. Offsets follow the RZ/V2H-family SYS layout (LSI_MODE / DEVID /
# PRR at 0x300 / 0x304 / 0x308). UNVERIFIED for RZ/V2N against the hardware
# manual -- every census value from here carries that marker.
SYS_REGS = {"soc_sys_lsi_mode": 0x10430300, "soc_lsi_devid": 0x10430304, "soc_prr": 0x10430308}
SYS_REGS_NOTE = "unverified addr"

ACT88760_ADDR = 0x25
ACT88760_GPIO_REG = 0x10
ACT88760_GPIO4_DEFECT = 0x88   # known-bad reg 0x10 default on some units (bench-only; see the private runbook)
ACT88760_GPIO4_RELEASE = 0x08  # volatile workaround, lost at power-off
DA9292_ADDR = 0x1E
DA9292_REGS = {
    "da9292_status": (0x00, 0x01),
    "da9292_ctrl": (0x06, 0x07, 0x08, 0x09),
    "da9292_vout": (0x0A, 0x0B, 0x0C, 0x0D),
    "da9292_ids": (0x19, 0x1A, 0x1B),
}
TPS628640_ADDRS = (0x44, 0x48, 0x4D, 0x4F)
TPS628640_VOUT1 = 0x01
RV3028_ADDR = 0x52
CLKGEN_5L35023B_ADDR = 0x69
# Bench-proven (2026-09-24/25) factory OTP image, reg 0x00..0x24 (37 bytes),
# read ONE BYTE AT A TIME (i2cget): a combined i2ctransfer read bit-slips on
# this part. 0x30..0x35 are live status, excluded from this table.
CLKGEN_OTP_IMAGE = bytes.fromhex(
    "a0 00 bb 04 32 08 cc 21 19 4c f2 16 5f 22 f0 3e 00 80 00 00 00 00 00 00 "
    "0e 0c 19 12 3f f0 90 46 a0 80 b0 b0 9c")
CLKGEN_REG_COUNT = len(CLKGEN_OTP_IMAGE)  # 0x25 (37): reg 0x00..0x24 inclusive
# U-Boot's 5L35023B fixup (U-Boot patch 0007, #2293) rewrites these two OTP
# registers every boot; a post-boot read must expect the fixed-up values, not
# the factory ones.
CLKGEN_FIXUP_REGS = {0x21: 0xC0, 0x24: 0x8E}
# DX-M1 (V2M-only) vendor firmware + the vendor uart_boot (aarch64) tool
# binary itself, all pinned by md5 -- see docs/provisioning-v2n.md.
DXM1_FW_UART_BOOT_MD5 = "ae449610ca72f4ebd431e4e2f7ebe0ab"
DXM1_FW_MD5 = "88641281169f35de5ed7e33c072c93a9"
DXM1_FW_VERSION = "2.4.0"
DXM1_UART_BOOT_TOOL_MD5 = "5971694fb5b616bffcadcc5e6d32c1db"

# preset i2c_devices bus name -> bench.yaml i2c_bus key
PRESET_BUS_TO_BENCH = {"e1m_i2c0": "eeprom", "brd_i2c": "brd"}


@dataclass
class CmdResult:
    rc: int
    stdout: str
    stderr: str


class LinuxTarget:
    def __init__(self, host: str, user: str = "root", runner=subprocess.run,
                 ssh: str = "ssh", scp: str = "scp") -> None:
        self.host = host
        self.user = user
        self.runner = runner
        self.ssh = ssh
        self.scp = scp
        self._opts = ["-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=accept-new"]

    def _exec(self, argv: list[str], timeout: float, stdin_path: Path | None = None) -> CmdResult:
        try:
            if stdin_path is None:
                p = self.runner(argv, capture_output=True, text=True, timeout=timeout)
            else:
                with open(stdin_path, "rb") as f:
                    p = self.runner(argv, stdin=f, capture_output=True, text=True, timeout=timeout)
        except subprocess.TimeoutExpired as e:
            raise BenchError(f"timed out after {timeout}s: {argv[-1]}") from e
        except OSError as e:
            raise BenchError(f"cannot run {argv[0]}: {e}") from e
        return CmdResult(p.returncode, p.stdout or "", p.stderr or "")

    def run(self, cmd: str, timeout: float = 60.0, check: bool = True,
            stdin_path: Path | None = None) -> CmdResult:
        r = self._exec([self.ssh, *self._opts, f"{self.user}@{self.host}", cmd], timeout, stdin_path)
        if check and r.rc != 0:
            raise BenchError(f"rc={r.rc}: {cmd}: {r.stderr.strip()[-500:]}")
        return r

    def put(self, local: Path, remote: str) -> None:
        r = self._exec([self.scp, *self._opts, str(local), f"{self.user}@{self.host}:{remote}"], 600.0)
        if r.rc != 0:
            raise BenchError(f"scp {local} -> {remote} failed: {r.stderr.strip()[-500:]}")

    def get(self, remote: str, local: Path) -> None:
        r = self._exec([self.scp, *self._opts, f"{self.user}@{self.host}:{remote}", str(local)], 600.0)
        if r.rc != 0:
            raise BenchError(f"scp {remote} -> {local} failed: {r.stderr.strip()[-500:]}")

    def md5(self, path: str, offset: int = 0, size: int | None = None) -> str:
        # tail/head instead of dd skip_bytes: works with busybox and coreutils alike.
        src = f"tail -c +{offset + 1} {shlex.quote(path)}"
        if size is not None:
            src += f" | head -c {size}"
        out = self.run(f"{src} | md5sum", timeout=600.0).stdout.split()
        if not out or not re.fullmatch(r"[0-9a-f]{32}", out[0]):
            raise BenchError(f"unparsable md5sum output for {path}")
        return out[0]


def _host_md5(path: Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


# --- console -----------------------------------------------------------------

_SHELL = r"(?:^|\n)[^\n]*[#$] $"


def console_login(console: Console, user: str = "root", timeout: float = 120.0) -> None:
    """Get a shell prompt on the Linux console (passwordless dev images only)."""
    console.send_line("")
    key, _ = console.expect_any({"login": r"login: *$", "shell": _SHELL}, timeout)
    if key == "shell":
        return
    console.send_line(user)
    key, _ = console.expect_any({"password": r"[Pp]assword: *$", "shell": _SHELL}, 30.0)
    if key == "password":
        raise BenchError(f"console login for {user!r} asks for a password; only passwordless images are supported")


def discover_host(console: Console, iface: str | None = None) -> str:
    """First global-scope IPv4 address, read over the logged-in console."""
    dev = f" dev {iface}" if iface else ""
    console.send_line(f"ip -4 -o addr show scope global{dev}")
    m = console.expect(r"inet (\d{1,3}(?:\.\d{1,3}){3})/", 10.0)
    console.drain()
    return m.group(1)


# --- block devices -------------------------------------------------------------

def resolve_emmc(t: LinuxTarget) -> str:
    """The eMMC by sysfs type, never by index (SD/eMMC numbering is not stable)."""
    out = t.run('for d in /sys/block/mmcblk*; do [ -r "$d/device/type" ] && '
                'echo "${d##*/} $(cat "$d/device/type")"; done; true').stdout
    # mmcblkNbootM / rpmb share the card's device/type -- only whole disks count
    hits = [n for n, typ in (ln.split(None, 1) for ln in out.splitlines() if " " in ln)
            if typ.strip() == "MMC" and re.fullmatch(r"mmcblk\d+", n)]
    if len(hits) != 1:
        raise BenchError(f"expected exactly one eMMC (device/type == MMC), found {hits or 'none'}")
    return f"/dev/{hits[0]}"


def root_device(t: LinuxTarget) -> str:
    """Block device mounted at / (via major:minor, so /dev/root is resolved)."""
    majmin = t.run("mountpoint -d /").stdout.strip()
    if not re.fullmatch(r"\d+:\d+", majmin):
        raise BenchError(f"unparsable mountpoint -d / output: {majmin!r}")
    path = t.run(f"readlink -f /sys/dev/block/{majmin}").stdout.strip()
    return "/dev/" + path.rsplit("/", 1)[-1]


# --- xSPI (MTD) ------------------------------------------------------------------

def mtd_erasesize(t: LinuxTarget, mtd: int) -> int:
    return int(t.run(f"cat /sys/class/mtd/mtd{mtd}/erasesize").stdout.strip())


def mtd_write_verify(t: LinuxTarget, mtd: int, local: Path, limit: int | None = None) -> str:
    """Erase ceil(size/erasesize) blocks from 0, write, read back, md5-compare."""
    data_len = local.stat().st_size
    if data_len == 0:
        raise ValueError(f"{local} is empty")
    es = mtd_erasesize(t, mtd)
    blocks = -(-data_len // es)
    if limit is not None and blocks * es > limit:
        raise ValueError(f"{local.name}: erase of {blocks * es:#x} bytes on mtd{mtd} would reach {limit:#x}")
    part = int(t.run(f"cat /sys/class/mtd/mtd{mtd}/size").stdout.strip())
    if blocks * es > part:
        raise ValueError(f"{local.name} ({data_len} bytes) does not fit mtd{mtd} ({part:#x})")
    want = _host_md5(local)
    remote = f"/tmp/{local.name}"
    t.put(local, remote)
    try:
        if t.md5(remote) != want:
            raise BenchError(f"{remote}: copy on the target does not match {local.name}")
        dev = f"/dev/mtd{mtd}"
        t.run(f"flash_erase {dev} 0 {blocks}", timeout=600.0)
        t.run(f"mtd_debug write {dev} 0 {data_len} {shlex.quote(remote)}", timeout=600.0)
        got = t.md5(dev, 0, data_len)
    finally:
        t.run(f"rm -f {shlex.quote(remote)}", check=False)
    if got != want:
        raise BenchError(f"mtd{mtd} readback md5 {got} != {local.name} md5 {want}")
    return got


# --- eMMC boot area + EXT_CSD ------------------------------------------------------

def emmc_boot1_write_verify(t: LinuxTarget, emmc: str, local: Path, sector: int) -> str:
    """dd into <emmc>boot1 at `sector` (512 B), force_ro cleared only for the write."""
    data_len = local.stat().st_size
    want = _host_md5(local)
    name = emmc.rsplit("/", 1)[-1]
    force_ro = f"/sys/block/{name}boot1/force_ro"
    dev = f"{emmc}boot1"
    remote = f"/tmp/{local.name}"
    t.put(local, remote)
    try:
        if t.md5(remote) != want:
            raise BenchError(f"{remote}: copy on the target does not match {local.name}")
        t.run(f"echo 0 > {force_ro}")
        try:
            t.run(f"dd if={shlex.quote(remote)} of={dev} bs=512 seek={sector} conv=fsync status=none",
                  timeout=600.0)
        finally:
            t.run(f"echo 1 > {force_ro}", check=False)
        got = t.md5(dev, sector * 512, data_len)
    finally:
        t.run(f"rm -f {shlex.quote(remote)}", check=False)
    if got != want:
        raise BenchError(f"{dev} sector {sector:#x} readback md5 {got} != {local.name} md5 {want}")
    return got


# mmc-utils prints fields by name; the byte indices we care about.
_EXT_CSD_FIELDS = {
    "BOOT_WP": 173,
    "BOOT_BUS_CONDITIONS": 177,
    "BOOT_CONFIG_PROT": 178,
    "PARTITION_CONFIG": 179,
    "HS_TIMING": 185,
    "EXT_CSD_REV": 192,
}


def ext_csd(t: LinuxTarget, emmc: str) -> dict[int, int]:
    out = t.run(f"mmc extcsd read {emmc}").stdout
    regs = {_EXT_CSD_FIELDS[m[1]]: int(m[2], 16)
            for m in re.finditer(r"\[(\w+): (0x[0-9a-fA-F]+)\]", out) if m[1] in _EXT_CSD_FIELDS}
    for idx in (177, 179):
        if idx not in regs:
            raise BenchError(f"mmc extcsd read {emmc}: EXT_CSD[{idx}] not found in output")
    return regs


def set_boot_config(t: LinuxTarget, emmc: str) -> None:
    """[177]=0x02 (x8, SDR backward-compatible, reset to x1), [179]=0x08 (boot1, no ACK)."""
    t.run(f"mmc bootbus set single_backward x1 x8 {emmc}")
    t.run(f"mmc bootpart enable 1 0 {emmc}")
    regs = ext_csd(t, emmc)
    if regs[177] != 0x02 or regs[179] != 0x08:
        raise BenchError(f"EXT_CSD after write: [177]={regs[177]:#04x} [179]={regs[179]:#04x}, want 0x02 / 0x08")


# --- rootfs ----------------------------------------------------------------------------

def rootfs_write_verify(t: LinuxTarget, emmc: str, wic_gz: Path, timeout: float = 3600.0) -> str:
    """Stream the gzipped wic over SSH into the eMMC user area, then md5 the written span."""
    h = hashlib.md5()
    n = 0
    with gzip.open(wic_gz, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
            n += len(block)
    want = h.hexdigest()
    t.run(f"gunzip -c | dd of={emmc} bs=4M conv=fsync status=none", timeout=timeout, stdin_path=wic_gz)
    t.run(f"blockdev --rereadpt {emmc}")
    got = t.md5(emmc, 0, n)
    if got != want:
        raise BenchError(f"{emmc} readback md5 {got} != uncompressed {wic_gz.name} md5 {want}")
    return got


def rootfs_check(t: LinuxTarget, emmc: str, part: int, dtb: str) -> None:
    """fsck -n, read-only mount, and the FDT gate's dtb present under /boot."""
    dev = f"{emmc}p{part}"
    mnt = "/mnt/alp-provision-rootfs"
    t.run(f"fsck.ext4 -n {dev}", timeout=600.0)
    t.run(f"mkdir -p {mnt} && mount -o ro {dev} {mnt}")
    try:
        r = t.run(f"test -f {mnt}/boot/{shlex.quote(dtb)}", check=False)
    finally:
        t.run(f"umount {mnt}", check=False)
    if r.rc != 0:
        raise BenchError(f"{dev}: /boot/{dtb} missing")


# --- I2C -------------------------------------------------------------------------------

def _parse_bytes(out: str, n: int) -> bytes:
    toks = re.findall(r"0x([0-9a-fA-F]{2})\b", out)
    if len(toks) != n:
        raise BenchError(f"i2ctransfer returned {len(toks)} bytes, wanted {n}: {out.strip()[:200]}")
    return bytes(int(x, 16) for x in toks)


def _xfer(t: LinuxTarget, bus: int, addr: int, write: bytes, read_len: int, check: bool = True) -> bytes | None:
    """ONE i2ctransfer invocation: optional write msg, optional repeated-start read."""
    if not write and not read_len:
        raise ValueError("empty i2c transfer")
    msgs = []
    if write:
        msgs.append(f"w{len(write)}@{addr:#04x} " + " ".join(f"{b:#04x}" for b in write))
    if read_len:
        msgs.append(f"r{read_len}" if write else f"r{read_len}@{addr:#04x}")
    r = t.run(f"i2ctransfer -y {bus} " + " ".join(msgs), check=check)
    if r.rc != 0:
        return None
    return _parse_bytes(r.stdout, read_len) if read_len else b""


def i2c_transfer(t: LinuxTarget, bus: int, frame) -> bytes:
    """Run one gates.I2cFrame. 0x58 traffic only from gates.identity_frame() (sealed)."""
    if frame.addr == IDENTITY_ADDR:
        if not frame.sealed:
            raise ValueError("refusing a 0x58 transfer not built by gates.identity_frame()")
        # Belt and braces for the selector table: a data-bearing 0x06 frame
        # rewrites the Device Configuration Register (can move / write-protect the array).
        if frame.write[:1] == b"\x06" and (len(frame.write) > 2 or frame.read_len == 0):
            raise ValueError("refusing a 0x58 selector 0x06 write")
    if frame.addr == EEPROM_ADDR:
        raise ValueError("0x50 traffic goes through eeprom_read / eeprom_write_pages")
    return _xfer(t, bus, frame.addr, bytes(frame.write), frame.read_len)


def _eeprom_span(offset: int, n: int) -> None:
    if n <= 0 or offset < 0 or offset + n > EEPROM_SIZE:
        raise ValueError(f"EEPROM span {offset:#x}+{n} outside 0..{EEPROM_SIZE:#x}")


def eeprom_read(t: LinuxTarget, bus: int, offset: int, n: int) -> bytes:
    _eeprom_span(offset, n)
    return _xfer(t, bus, EEPROM_ADDR, bytes((offset >> 8, offset & 0xFF)), n)


def eeprom_write_pages(t: LinuxTarget, bus: int, offset: int, data: bytes,
                       page: int = 16, poll_ms: int = 50) -> None:
    """Chunked page writes, each followed by an on-target ACK poll, then a full readback."""
    _eeprom_span(offset, len(data))
    if page <= 0 or EEPROM_DEVICE_PAGE % page:
        raise ValueError(f"chunk {page} must divide the {EEPROM_DEVICE_PAGE}-byte device page")
    pos = 0
    while pos < len(data):
        off = offset + pos
        take = min(page - off % page, len(data) - pos)   # never crosses a device page
        hi, lo = off >> 8, off & 0xFF
        payload = " ".join(f"{b:#04x}" for b in (hi, lo, *data[pos:pos + take]))
        # ponytail: poll bound counts attempts (each i2ctransfer spawn >= ~1 ms), not wall time
        t.run(f"i2ctransfer -y {bus} w{take + 2}@{EEPROM_ADDR:#04x} {payload} || exit 2; "
              f"n=0; until i2ctransfer -y {bus} w2@{EEPROM_ADDR:#04x} {hi:#04x} {lo:#04x} "
              f">/dev/null 2>&1; do n=$((n+1)); [ $n -ge {poll_ms} ] && exit 3; done")
        pos += take
    got = eeprom_read(t, bus, offset, len(data))
    if got != data:
        bad = next(i for i in range(len(data)) if got[i] != data[i])
        raise BenchError(f"EEPROM readback differs at {offset + bad:#06x}: {got[bad]:#04x} != {data[bad]:#04x}")


def secure_page_write_verify(t: LinuxTarget, bus: int, write_frame, read_frame,
                             poll: int = 50) -> bytes:
    """Write the 64-byte Secure Data Page, poll with the read frame until it ACKs,
    compare. Both frames come from gates.identity_frame(); lock is NOT done here."""
    expected = bytes(write_frame.write[2:])
    i2c_transfer(t, bus, write_frame)
    for _ in range(poll):   # write cycle: the read frame NACKs until it completes
        try:
            got = i2c_transfer(t, bus, read_frame)
            break
        except BenchError:
            continue
    else:
        raise BenchError(f"secure page did not ACK after {poll} polls")
    if got != expected:
        raise BenchError("secure page readback differs from the written 64 bytes")
    return got


def i2c_get(t: LinuxTarget, bus: int, addr: int, reg: int) -> int:
    # -f: read even when a kernel driver owns the address (e.g. the RTC shows UU)
    out = t.run(f"i2cget -y -f {bus} {addr:#04x} {reg:#04x}").stdout.strip()
    if not re.fullmatch(r"0x[0-9a-fA-F]{2}", out):
        raise BenchError(f"i2cget {bus} {addr:#04x} {reg:#04x}: unparsable {out!r}")
    return int(out, 16)


def i2c_set(t: LinuxTarget, bus: int, addr: int, reg: int, value: int) -> None:
    if addr in (EEPROM_ADDR, IDENTITY_ADDR, CLKGEN_5L35023B_ADDR):
        raise ValueError(f"i2c_set refuses {addr:#04x}: EEPROM/clkgen traffic only through their "
                         "dedicated helpers (the 5L35023B OTP image cannot be re-burned in-system)")
    if not 0 <= value <= 0xFF or not 0 <= reg <= 0xFF:
        raise ValueError(f"reg/value out of range: {reg:#x}/{value:#x}")
    t.run(f"i2cset -y {bus} {addr:#04x} {reg:#04x} {value:#04x}")


def i2c_scan(t: LinuxTarget, bus: int) -> set[int]:
    """i2cdetect -r (read-byte probe, no quick-write); UU counts as present."""
    out = t.run(f"i2cdetect -y -r {bus}").stdout
    found: set[int] = set()
    for ln in out.splitlines():
        m = re.match(r"([0-7]0):\s(.*)", ln)
        if not m:
            continue
        base = int(m[1], 16)
        for i, cell in enumerate(m[2].split()):
            if cell == "UU" or re.fullmatch(r"[0-9a-f]{2}", cell):
                found.add(base + i)
    return found


def act88760_gpio4_defect(t: LinuxTarget, bus: int) -> bool:
    return i2c_get(t, bus, ACT88760_ADDR, ACT88760_GPIO_REG) == ACT88760_GPIO4_DEFECT


def act88760_gpio4_release(t: LinuxTarget, bus: int) -> None:
    """Volatile workaround (lost at power-off): release GD32_NRST. Only on a defect unit."""
    if not act88760_gpio4_defect(t, bus):
        raise BenchError(f"ACT88760 reg {ACT88760_GPIO_REG:#04x} is not {ACT88760_GPIO4_DEFECT:#04x}; refusing to write")
    i2c_set(t, bus, ACT88760_ADDR, ACT88760_GPIO_REG, ACT88760_GPIO4_RELEASE)
    got = i2c_get(t, bus, ACT88760_ADDR, ACT88760_GPIO_REG)
    if got != ACT88760_GPIO4_RELEASE:
        raise BenchError(f"ACT88760 reg {ACT88760_GPIO_REG:#04x} reads {got:#04x} after writing {ACT88760_GPIO4_RELEASE:#04x}")


def expected_i2c(preset: dict, i2c_bus: dict[str, int]) -> dict[int, set[int]]:
    """Required (non-optional) on-module addresses per Linux bus number, from the preset."""
    want: dict[int, set[int]] = {}
    for name, spec in (preset.get("on_module", {}).get("i2c_devices") or {}).items():
        bench_key = PRESET_BUS_TO_BENCH.get(name)
        if bench_key is None or bench_key not in i2c_bus:
            continue
        addrs = want.setdefault(i2c_bus[bench_key], set())
        for d in spec.get("devices", []):
            if d.get("assembled") != "optional":
                addrs.add(int(str(d["address_7bit"]), 16))
    return want


def i2c_check(t: LinuxTarget, expected: dict[int, set[int]]) -> list[str]:
    """Missing required devices per bus ([] = all present). Extra ACKs are carrier
    devices (E1M_I2C0 leaves the module) and are not a failure."""
    problems = []
    for bus, want in sorted(expected.items()):
        missing = want - i2c_scan(t, bus)
        if missing:
            problems.append(f"i2c-{bus}: missing " + " ".join(f"{a:#04x}" for a in sorted(missing)))
    return problems


# --- clock generator (5L35023B) ---------------------------------------------------------

def clkgen_read_image(t: LinuxTarget, bus: int) -> bytes:
    """Read reg 0x00..0x24 ONE BYTE AT A TIME (i2cget): a combined
    i2ctransfer read bit-slips on this part."""
    return bytes(i2c_get(t, bus, CLKGEN_5L35023B_ADDR, reg) for reg in range(CLKGEN_REG_COUNT))


def _clkgen_expected() -> bytes:
    img = bytearray(CLKGEN_OTP_IMAGE)
    for reg, val in CLKGEN_FIXUP_REGS.items():
        img[reg] = val
    return bytes(img)


def clkgen_diff(image: bytes) -> list[str]:
    """Mismatches of `image` against the OTP image with the U-Boot fixup
    applied. [] means the image is exactly what a provisioned unit should read."""
    if len(image) != CLKGEN_REG_COUNT:
        raise ValueError(f"clkgen image must be {CLKGEN_REG_COUNT} bytes, got {len(image)}")
    want = _clkgen_expected()
    return [f"reg {i:#04x}: {image[i]:#04x} != {want[i]:#04x}"
            for i in range(CLKGEN_REG_COUNT) if image[i] != want[i]]


def clkgen_fixup_applied(image: bytes) -> bool:
    return all(image[reg] == val for reg, val in CLKGEN_FIXUP_REGS.items())


def clkgen_otp_sha256(image: bytes) -> str:
    """sha256 of the 37-byte image with reg 0x21/0x24 normalized back to
    their factory OTP values (the U-Boot fixup rewrites both every boot, so
    the raw post-boot bytes would otherwise hash differently unit to unit)."""
    img = bytearray(image)
    for reg in CLKGEN_FIXUP_REGS:
        img[reg] = CLKGEN_OTP_IMAGE[reg]
    return hashlib.sha256(bytes(img)).hexdigest()


# --- DX-M1 NPU (V2M-only): SPI-NAND recovery boot over UART -------------------------------

PCIE_ROOT_PORT = "0000:00:00.0"


def _gpioset_is_v2(t: LinuxTarget) -> bool:
    """libgpiod v2's `gpioset` takes `-c <chip> -z <line>=<val>` (background hold
    while the process runs); v1 takes a positional chip and needs
    `--mode=signal` to hold instead of setting-then-exiting."""
    out = t.run("gpioset --version", check=False).stdout
    m = re.search(r"(\d+)\.\d+", out)
    return bool(m) and int(m[1]) >= 2


def dxm1_uart_boot(t: LinuxTarget, chip: str, uart_line: int, reset_line: int,
                   uart_device: str, tool: str, fw_uart_boot: str, fw: str) -> str:
    """Mux V2N P75 to the DX-M1 UART0 (held high, backgrounded, for the whole
    transfer -- a plain foreground `gpioset` sets the line then exits and
    releases it), pulse the PA6 reset (low 100 ms, high), then run the
    vendor `uart_boot` twice against the ROM's XMODEM fallback ('C' prompt on
    an empty NAND): once for the bootloader stage, once for the application
    firmware (`-d <uart_device>` on both -- the vendor tool needs the device
    path for either transfer mode). Kills the P75 hold before returning
    (even on failure). Never touches V2N P64/P65 (the DEEPX 0.75 V rail):
    this mechanism is a UART mux line and a reset line only."""
    hold = (f"gpioset -c {shlex.quote(chip)} -z {uart_line}=1" if _gpioset_is_v2(t) else
            f"gpioset --mode=signal {shlex.quote(chip)} {uart_line}=1")
    pid = t.run(f"{hold} >/dev/null 2>&1 & echo $!").stdout.strip()
    if not pid.isdigit():
        raise BenchError(f"could not start the P75 hold ({hold!r}): got {pid!r}")
    try:
        t.run(f"gpioset {shlex.quote(chip)} {reset_line}=0; sleep 0.1; "
              f"gpioset {shlex.quote(chip)} {reset_line}=1")
        out1 = t.run(f"{shlex.quote(tool)} -d {shlex.quote(uart_device)} "
                     f"-f {shlex.quote(fw_uart_boot)} -b 115200", timeout=120.0).stdout
        out2 = t.run(f"{shlex.quote(tool)} -d {shlex.quote(uart_device)} "
                     f"-F {shlex.quote(fw)} -U -b 115200", timeout=300.0).stdout
    finally:
        t.run(f"kill {shlex.quote(pid)}", check=False)
    return out1 + out2


def dxm1_pcie_present(t: LinuxTarget, vendor_id: str | None = None) -> bool:
    """A DEEPX PCIe *endpoint* is enumerated, not just the SoC's own root
    port: every boot lists the root port itself (`0000:00:00.0`); at least
    one OTHER entry must be present. When `vendor_id` is given (bench.yaml
    `dxm1.pcie_vendor_id`, e.g. "0x1f4b"), an endpoint must also match it via
    `/sys/bus/pci/devices/*/vendor`. DEEPX has not published the DX-M1's PCI
    vendor/device ID in any vendor material seen so far; until a bench.yaml
    supplies one, any non-root-port entry counts."""
    out = t.run("ls /sys/bus/pci/devices", check=False).stdout.split()
    endpoints = [d for d in out if d != PCIE_ROOT_PORT]
    if not endpoints:
        return False
    if vendor_id is None:
        return True
    want = vendor_id.lower()
    for d in endpoints:
        r = t.run(f"cat /sys/bus/pci/devices/{shlex.quote(d)}/vendor", check=False)
        if r.rc == 0 and r.stdout.strip().lower() == want:
            return True
    return False


# --- census ----------------------------------------------------------------------------

def parse_emmc_cid(raw: str) -> dict[str, str]:
    raw = raw.strip().lower()
    if not re.fullmatch(r"[0-9a-f]{32}", raw):
        raise ValueError(f"CID must be 32 hex chars, got {raw!r}")
    b = bytes.fromhex(raw)
    return {
        "emmc_cid_raw": raw,
        "emmc_cid_mid": f"{b[0]:#04x}",
        "emmc_cid_pnm": b[3:9].decode("ascii", "replace").rstrip("\x00 "),
        "emmc_cid_prv": f"{b[9]:#04x}",
        "emmc_cid_psn": f"0x{int.from_bytes(b[10:14], 'big'):08x}",
        "emmc_cid_mdt": f"{b[14]:#04x}",
    }


def _hexbytes(b: bytes) -> str:
    return " ".join(f"{x:02x}" for x in b)


def _reg_dump(t: LinuxTarget, bus: int, addr: int, regs) -> str:
    return " ".join(f"{r:#04x}={i2c_get(t, bus, addr, r):#04x}" for r in regs)


def _devmem(t: LinuxTarget, addr: int) -> int:
    page, off = addr & ~0xFFF, addr & 0xFFF
    py = ("import mmap,os,struct;f=os.open('/dev/mem',os.O_RDONLY|os.O_SYNC);"
          f"m=mmap.mmap(f,4096,mmap.MAP_SHARED,mmap.PROT_READ,offset={page});"
          f"print(hex(struct.unpack_from('<I',m,{off})[0]))")
    out = t.run(f"devmem {addr:#x} 32 2>/dev/null || python3 -c {shlex.quote(py)}").stdout.strip()
    return int(out, 16)


def census(t: LinuxTarget, i2c_bus: dict[str, int], sizes: dict[str, int] | None = None,
           emmc: str | None = None) -> tuple[dict[str, str], list[str]]:
    """Read-only. Returns (auto ledger keys, notes on what could not be read).

    `sizes` = artefact byte lengths keyed by bundle role ("bl2", "fip",
    "bl2_mmc", "cm33"); an md5 key is produced only when its size is known.
    Never issues a write to the unit (the only 0x58 frames are the sealed reads).
    """
    from provision import gates   # lazy: gates is the single source of 0x58 frames

    sizes = sizes or {}
    facts: dict[str, str] = {}
    notes: list[str] = []

    def group(name, fn):
        try:
            fn()
        except (BenchError, ValueError) as e:
            notes.append(f"{name}: {e}")

    def soc():
        for key, addr in SYS_REGS.items():
            facts[key] = f"{_devmem(t, addr):#x} ({SYS_REGS_NOTE} {addr:#x})"

    def cpu_mem():
        facts["cpu_khz"] = t.run("cat /sys/devices/system/cpu/cpu0/cpufreq/cpuinfo_max_freq").stdout.strip()
        m = re.search(r"^MemTotal:\s+(\d+) kB", t.run("cat /proc/meminfo").stdout, re.M)
        if not m:
            raise BenchError("MemTotal not in /proc/meminfo")
        facts["linux_memtotal_kb"] = m[1]
        facts["kernel_version"] = t.run("uname -r").stdout.strip()

    def storage():
        dev = emmc or resolve_emmc(t)
        name = dev.rsplit("/", 1)[-1]
        facts.update(parse_emmc_cid(t.run(f"cat /sys/block/{name}/device/cid").stdout))
        facts["emmc_size_bytes"] = str(int(t.run(f"cat /sys/block/{name}/size").stdout.strip()) * 512)
        regs = ext_csd(t, dev)
        facts["emmc_ext_csd_177"] = f"{regs[177]:#04x}"
        facts["emmc_ext_csd_179"] = f"{regs[179]:#04x}"
        if "bl2_mmc" in sizes:
            facts["emmc_boot1_bl2_md5"] = t.md5(f"{dev}boot1", 1 * 512, sizes["bl2_mmc"])
        if "fip" in sizes:
            facts["emmc_boot1_fip_md5"] = t.md5(f"{dev}boot1", 0x300 * 512, sizes["fip"])
        ios = t.run(f'cat /sys/kernel/debug/$(basename $(dirname $(readlink -f /sys/block/{name}/device)))/ios',
                    check=False).stdout
        m = re.search(r"timing spec:\s*\d+ \(([^)]+)\)", ios)
        c = re.search(r"actual clock:\s*(\d+) Hz", ios)
        if m:
            facts["emmc_mode"] = m[1] + (f" {c[1]} Hz" if c else "")
        else:
            notes.append("emmc_mode: mmc ios not readable (debugfs not mounted?)")

    def xspi():
        jedec = t.run("cat /sys/bus/spi/devices/*/spi-nor/jedec_id 2>/dev/null | head -n1", check=False).stdout.strip()
        if re.fullmatch(r"[0-9a-f]{6,}", jedec):
            facts["xspi_jedec_id"] = "0x" + jedec
        else:
            notes.append("xspi_jedec_id: no spi-nor/jedec_id in sysfs")
        # ponytail: sum of NOR partitions = flash size only when the partitions
        # tile the whole part; read the SFDP density if they ever stop doing so.
        out = t.run('for m in /sys/class/mtd/mtd*; do [ "$(cat $m/type)" = nor ] && cat $m/size; done; true').stdout
        total = sum(int(x) for x in out.split())
        if total:
            facts["xspi_size_bytes"] = str(total)
        for key, mtd, off, role in (("xspi_bl2_md5", 0, 0, "bl2"), ("xspi_fip_md5", 1, 0, "fip"),
                                    ("xspi_cm33_md5", 1, CM33_REGION_OFFSET, "cm33")):
            if role in sizes:
                facts[key] = t.md5(f"/dev/mtd{mtd}", off, sizes[role])

    def identity():
        bus = i2c_bus["eeprom"]
        rd = lambda op: i2c_transfer(t, bus, gates.identity_frame(op))  # noqa: E731
        facts["eeprom_unique_id"] = _hexbytes(rd(gates.IdentityOp.UNIQUE_ID_READ))
        lock = rd(gates.IdentityOp.LOCK_STATUS_READ)[0]
        facts["eeprom_lock_status"] = f"{lock:#04x}"
        facts["eeprom_device_config"] = f"{rd(gates.IdentityOp.DEVICE_CONFIG_READ)[0]:#04x}"
        page = rd(gates.IdentityOp.SECURE_PAGE_READ)
        facts["secure_page_sha256"] = hashlib.sha256(page).hexdigest()
        if lock & 0x02:
            facts["secure_page_state"] = "locked"
        elif page == b"\xff" * 64:
            facts["secure_page_state"] = "blank"
        # written-but-unlocked is "written-verified" only when secure_page compared it
        arr = eeprom_read(t, bus, 0, MANIFEST_LEN)
        if arr != b"\xff" * MANIFEST_LEN:
            facts["manifest_sha256"] = hashlib.sha256(arr).hexdigest()
            facts["manifest_crc32"] = f"0x{int.from_bytes(arr[0x7C:0x80], 'little'):08x}"
            if zlib.crc32(arr[:0x7C]) != int.from_bytes(arr[0x7C:0x80], "little"):
                notes.append("manifest_crc32: stored CRC does not match bytes 0x00..0x7b")

    def power():
        pmic = i2c_bus["pmic"]
        facts["act88760_gpio_regs"] = _reg_dump(t, pmic, ACT88760_ADDR, (ACT88760_GPIO_REG,))
        for key, regs in DA9292_REGS.items():
            facts[key] = _reg_dump(t, pmic, DA9292_ADDR, regs)
        present = sorted(i2c_scan(t, pmic) & set(TPS628640_ADDRS))
        facts["tps_present"] = " ".join(f"{a:#04x}" for a in present) or "none"
        if present:
            facts["tps_vout"] = " ".join(f"{a:#04x}={i2c_get(t, pmic, a, TPS628640_VOUT1):#04x}" for a in present)

    def clocks_rtc():
        brd = i2c_bus["brd"]
        facts["rtc_rv3028_reg_0x37"] = f"{i2c_get(t, brd, RV3028_ADDR, 0x37):#04x}"
        facts["clkgen_5l35023b_regs"] = ("ack" if CLKGEN_5L35023B_ADDR in i2c_scan(t, brd) else "no ack") + \
            f" at {CLKGEN_5L35023B_ADDR:#04x}"

    def network():
        for i in (0, 1):
            r = t.run(f"cat /sys/class/net/eth{i}/address /sys/class/net/eth{i}/operstate", check=False)
            lines = r.stdout.split()
            if r.rc == 0 and len(lines) == 2:
                facts[f"eth{i}_mac"], facts[f"eth{i}_link"] = lines
            else:
                notes.append(f"eth{i}: not present")

    for name, fn in (("soc", soc), ("cpu_mem", cpu_mem), ("storage", storage), ("xspi", xspi),
                     ("identity", identity), ("power", power), ("clocks_rtc", clocks_rtc),
                     ("network", network)):
        group(name, fn)
    return facts, notes
