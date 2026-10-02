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
import tempfile
import time
import zlib
from dataclasses import dataclass
from pathlib import Path
from typing import TYPE_CHECKING

from provision import bmap, payload_store
from provision.bench import BenchError, ExpectTimeout
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
# OTP GPIO4 (GD32_NRST) default. Most units' factory OTP already holds this
# reg at 0x08; a few early units (e.g. E1M-V2M103 2026W38-0001) have the OTP
# default 0x88, which holds the GD32 in reset until released -- an expected,
# known-workaround condition, not a defect (maintainer decision 2026-09-29).
# U-Boot's board_late_init releases it every boot (a no-op on an already-0x08
# unit); a provisioning tool run that still finds 0x88 applies the same
# volatile release itself, so provisioning never depends on the U-Boot fix
# having landed yet.
ACT88760_GPIO4_OTP_DEFAULT = 0x88
ACT88760_GPIO4_RELEASED = 0x08  # released (by U-Boot or the tool); volatile, lost at power-off
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
        # Units get reused DHCP addresses and the SD and eMMC images carry different host
        # keys by design, so host keys identify nothing here; unit identity is the eMMC CID
        # check in Ctx.need_linux.
        self._opts = ["-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=no",
                      "-o", "UserKnownHostsFile=/dev/null", "-o", "ConnectTimeout=5",
                      "-o", "LogLevel=ERROR"]

    def _exec(self, argv: list[str], timeout: float, stdin_path: Path | None = None) -> CmdResult:
        try:
            if stdin_path is None:
                p = self.runner(argv, stdin=subprocess.DEVNULL, capture_output=True, text=True, timeout=timeout)
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
        # Plain dd only: busybox builds may lack `head -c` and dd's
        # skip_bytes/count_bytes (the alp-image-edge busybox has neither; a
        # missing `head -c` hashed an EMPTY stream and every probe read as a
        # mismatch).  Whole blocks first, then the < bs remainder at bs=1.
        q = shlex.quote(path)
        bs = 4096 if offset % 4096 == 0 else 512 if offset % 512 == 0 else 1
        if size is None:
            src = f"dd if={q} bs={bs} skip={offset // bs} 2>/dev/null"
        else:
            whole, rem = divmod(size, bs)
            parts = ([f"dd if={q} bs={bs} skip={offset // bs} count={whole} 2>/dev/null"] if whole else []) +                     ([f"dd if={q} bs=1 skip={offset + whole * bs} count={rem} 2>/dev/null"] if rem else [])
            src = parts[0] if len(parts) == 1 else "{ " + "; ".join(parts or ["true"]) + "; }"
        out = self.run(f"{src} | md5sum", timeout=600.0).stdout.split()
        if not out or not re.fullmatch(r"[0-9a-f]{32}", out[0]):
            raise BenchError(f"unparsable md5sum output for {path}")
        if size and out[0] == _EMPTY_MD5:
            raise BenchError(f"read 0 of {size} bytes from {path} at {offset:#x}")
        return out[0]


_EMPTY_MD5 = hashlib.md5(b"").hexdigest()


def _host_md5(path: Path) -> str:
    h = hashlib.md5()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


# --- console -----------------------------------------------------------------

_SHELL = r"(?:^|\n)[^\n]*[#$] $"


_CPR_QUERY = "\x1b[6n"       # "report the cursor position": the image's profile runs a resize
_CPR_REPLY = b"\x1b[24;80R"
CONSOLE_SETTLE_S = 0.5


def _expect_answering_cpr(console: Console, patterns: dict[str, str], timeout: float) -> str:
    """expect_any(), but a cursor-position query on the way is answered (an
    unanswered resize leaves the shell busy and garbles the next commands)."""
    deadline = time.monotonic() + timeout
    pats = {**patterns, "cpr": re.escape(_CPR_QUERY)}
    while True:
        key, _ = console.expect_any(pats, max(deadline - time.monotonic(), 0.1))
        if key != "cpr":
            return key
        console.write(_CPR_REPLY)


def _settle(console: Console) -> None:
    """Let the login scripts finish: answer any late cursor query, wait for quiet."""
    while True:
        text = console.drain(CONSOLE_SETTLE_S, 10.0)
        if _CPR_QUERY not in text:
            return
        console.write(_CPR_REPLY)


def send_checked(console: Console, line: str, echo_timeout: float = 5.0) -> None:
    """Send one command line paced, and verify the tty echoed it intact (head of
    the line). On a mismatch: Ctrl-C, let the prompt settle, resend once."""
    head = re.escape(line.split("\n")[0][:40])
    for attempt in (1, 2):
        console.send_line(line, paced=True)
        try:
            console.expect(head, echo_timeout)
            return
        except ExpectTimeout as e:
            if attempt == 2:
                raise BenchError(f"console echo of {line[:40]!r} never matched what was sent "
                                 f"(corrupted RX?): {e.tail[-120:]!r}") from e
            console.write(b"\x03")
            console.drain(CONSOLE_SETTLE_S, 5.0)


SYSTEM_SETTLE_CAP_S = 90.0
SYSTEM_POLL_S = 2.0


def wait_system_settled(console: Console, cap: float | None = None) -> bool:
    """Poll ``systemctl is-system-running`` (paced, cheap) until it says running or
    degraded. Right after login systemd is still starting units (logind restarting)
    and the console RX can corrupt a byte; commands typed minutes later are clean.
    Returns False when the cap ran out (the caller proceeds anyway) and True otherwise;
    an image without systemctl counts as settled."""
    deadline = time.monotonic() + (SYSTEM_SETTLE_CAP_S if cap is None else cap)
    pats = {
        "done": r"(?m)^(?:running|degraded)\r?$",
        "busy": r"(?m)^(?:starting|initializing|stopping|offline|maintenance)\r?$",
        "none": r"not found|No such file",
    }
    while time.monotonic() < deadline:
        try:
            send_checked(console, "systemctl is-system-running")
            key, _ = console.expect_any(pats, 10.0)
        except (ExpectTimeout, BenchError):
            key = "busy"
        if key in ("done", "none"):
            console.drain(CONSOLE_SETTLE_S, 5.0)
            return True
        time.sleep(SYSTEM_POLL_S)
        console.drain(CONSOLE_SETTLE_S, 5.0)
    return False


def console_login(console: Console, user: str = "root", timeout: float = 120.0) -> None:
    """Get a shell prompt on the Linux console (passwordless dev images only)."""
    console.send_line("")
    key = _expect_answering_cpr(console, {"login": r"login: *$", "shell": _SHELL}, timeout)
    if key == "login":
        console.send_line(user, paced=True)
        key = _expect_answering_cpr(console, {"password": r"[Pp]assword: *$", "shell": _SHELL}, 30.0)
        if key == "password":
            raise BenchError(f"console login for {user!r} asks for a password; only passwordless images are supported")
    _settle(console)
    # no more escape-sequence queries from the shell or its profile
    send_checked(console, "export TERM=dumb")
    console.expect(_SHELL, 10.0)
    wait_system_settled(console)


def discover_host(console: Console, iface: str | None = None) -> str:
    """First global-scope IPv4 address, read over the logged-in console."""
    dev = f" dev {iface}" if iface else ""
    send_checked(console, f"ip -4 -o addr show scope global{dev}")
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


def mtd_write_verify(t: LinuxTarget, mtd: int, local: Path, limit: int | None = None, store=None) -> str:
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
    payload_store.stage(t, store, local, remote)
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

def emmc_boot1_write_verify(t: LinuxTarget, emmc: str, local: Path, sector: int, store=None) -> str:
    """dd into <emmc>boot1 at `sector` (512 B), force_ro cleared only for the write."""
    data_len = local.stat().st_size
    want = _host_md5(local)
    name = emmc.rsplit("/", 1)[-1]
    force_ro = f"/sys/block/{name}boot1/force_ro"
    dev = f"{emmc}boot1"
    # boot1 is a few MiB: refuse before writing anything rather than leave a
    # new BL2 next to a half-written FIP when the image runs off the end.
    part = int(t.run(f"cat /sys/block/{name}boot1/size").stdout.strip()) * 512
    if sector * 512 + data_len > part:
        raise BenchError(f"{local.name} ({data_len} B) at sector {sector:#x} does not fit "
                         f"{dev} ({part} B)")
    remote = f"/tmp/{local.name}"
    payload_store.stage(t, store, local, remote)
    try:
        if t.md5(remote) != want:
            raise BenchError(f"{remote}: copy on the target does not match {local.name}")
        t.run(f"echo 0 > {force_ro}")
        try:
            # no conv=/status= operands: a minimal busybox dd rejects them outright
            t.run(f"dd if={shlex.quote(remote)} of={dev} bs=512 seek={sector} && sync",
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

def rootfs_write_verify(t: LinuxTarget, emmc: str, wic_gz: Path, timeout: float = 3600.0, store=None) -> dict[str, str]:
    """Stream the gzipped wic over SSH into the eMMC user area, then md5 the written span."""
    h = hashlib.md5()
    n = 0
    with gzip.open(wic_gz, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
            n += len(block)
    want = h.hexdigest()
    local = store.fetch(wic_gz) if store is not None else None      # hash-verified on the board
    t.run(f"gunzip -c{' ' + shlex.quote(local) if local else ''} | dd of={emmc} bs=4M && sync",
          timeout=timeout, stdin_path=None if local else wic_gz)
    # busybox images may lack blockdev: fall back to the BLKRRPART ioctl
    t.run(f"blockdev --rereadpt {emmc} 2>/dev/null || python3 -c "
          f"\"import fcntl, os; fcntl.ioctl(os.open('{emmc}', os.O_RDONLY), 0x125f)\"")
    got = t.md5(emmc, 0, n)
    if got != want:
        raise BenchError(f"{emmc} readback md5 {got} != uncompressed {wic_gz.name} md5 {want}")
    return {"rootfs_md5": got, "rootfs_bytes_written": str(n), "rootfs_image_bytes": str(n)}


RANGES_PATH = "/tmp/alp-bmap-ranges"
WRITER = Path(__file__).with_name("bmap_writer.py")
WRITER_PATH = "/tmp/alp-bmap-writer.py"


def have_python3(t: LinuxTarget) -> bool:
    return t.run("command -v python3", check=False).rc == 0


def put_ranges(t: LinuxTarget, bm: bmap.Bmap) -> None:
    # A file, not argv: a wic has thousands of ranges.
    with tempfile.TemporaryDirectory() as d:
        f = Path(d) / "ranges"
        f.write_text("".join(f"{s} {c}\n" for s, c, _ in bm.ranges), encoding="ascii", newline="\n")
        t.put(f, RANGES_PATH)


def md5_ranges(t: LinuxTarget, dev: str, bm: bmap.Bmap) -> str:
    """md5 of the mapped ranges of dev, concatenated (the ranges file must be on the board)."""
    out = t.run(f"{{ while read s c <&3; do dd if={dev} bs={bm.block_size} skip=$s count=$c "
                f"2>/dev/null; done; }} 3<{RANGES_PATH} | md5sum", timeout=1800.0).stdout.split()
    if not out or not re.fullmatch(r"[0-9a-f]{32}", out[0]):
        raise BenchError(f"unparsable md5sum output for the mapped ranges of {dev}")
    return out[0]


_REREADPT = ("blockdev --rereadpt {emmc} 2>/dev/null || python3 -c "
             "\"import fcntl, os; fcntl.ioctl(os.open('{emmc}', os.O_RDONLY), 0x125f)\"")


def rootfs_write_verify_mapped(t: LinuxTarget, emmc: str, wic_gz: Path, bm: bmap.Bmap,
                               timeout: float = 3600.0, store=None) -> dict[str, str]:
    """Write only the bmap's mapped ranges (host-verified first), then md5 them back.

    The board runs bmap_writer.py (python3). With the wic.gz in the SD store it reads the WHOLE
    stream and skips the gaps between ranges; otherwise the host sends only the mapped bytes.
    When ImageSize is not a multiple of BlockSize the last block is zero-padded on BOTH paths
    (the host pads the stream it sends; the writer pads the short final block of the whole
    image), which is what the whole-block readback md5 expects."""
    local = store.fetch(wic_gz) if store is not None else None
    with tempfile.TemporaryDirectory() as d:
        staged = Path(d) / "mapped.gz"
        want, nbytes = bmap.stage(wic_gz, bm, None if local else staged)   # refuses on a checksum mismatch
        put_ranges(t, bm)
        t.put(WRITER, WRITER_PATH)
        w = f"python3 {WRITER_PATH} {emmc} {bm.block_size} {RANGES_PATH}"
        try:
            if local:
                t.run(f"{w} {bm.image_size} < {shlex.quote(local)}", timeout=timeout)
            else:
                t.run(f"{w} 0", timeout=timeout, stdin_path=staged)
            t.run(_REREADPT.format(emmc=emmc))
            got = md5_ranges(t, emmc, bm)
        finally:
            t.run(f"rm -f {RANGES_PATH} {WRITER_PATH}", check=False)
    if got != want:
        raise BenchError(f"{emmc} mapped-range readback md5 {got} != host md5 {want}")
    return {"rootfs_md5": got, "rootfs_bytes_written": str(nbytes), "rootfs_image_bytes": str(bm.image_size)}


def rootfs_check(t: LinuxTarget, emmc: str, part: int, dtb: str) -> None:
    """fsck -n, read-only mount, and the FDT gate's dtb present under /boot."""
    dev = f"{emmc}p{part}"
    mnt = "/mnt/alp-provision-rootfs"
    t.run(f"fsck.ext4 -n {dev}", timeout=600.0)
    t.run(f"mkdir -p {mnt} && mount -o ro,noload {dev} {mnt}")
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
              f">/dev/null 2>&1; do n=$((n+1)); if [ $n -ge {poll_ms} ]; then exit 3; fi; done")
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


def _crc16_ccitt_false(data: bytes) -> int:
    c = 0xFFFF
    for byte in data:
        c ^= byte << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
    return c


def gd32_bridge_version(t: LinuxTarget, bus: int, addr: int = 0x70) -> tuple[int, int, int]:
    """GET_VERSION over the bridge's I2C transport, read-only.

    Frame (kernel gpio-gd32-bridge 0005): write [0x00 reg][0x01 cmd][crc lo][crc hi],
    repeated-start read [status][major][minor][patch][crc lo][crc hi], CRC-16/CCITT-FALSE
    over cmd (+payload) / status+payload.  `-f` because the kernel driver binds 0x70.
    Asking the bridge beats grepping dmesg: the driver logs the protocol only if the
    GD32 answers AT PROBE, which a GD32 still held in reset (ACT88760 GPIO4 not yet
    released) never does."""
    crc = _crc16_ccitt_false(b"")
    r = t.run(f"i2ctransfer -f -y {bus} w4@{addr:#04x} 0x00 0x01 {crc & 0xFF:#04x} {crc >> 8:#04x} r6")
    rsp = _parse_bytes(r.stdout, 6)
    if _crc16_ccitt_false(rsp[:4]) != rsp[4] | rsp[5] << 8:
        raise BenchError(f"GD32 GET_VERSION reply CRC mismatch: {rsp.hex(' ')}")
    if rsp[0] != 0:
        raise BenchError(f"GD32 GET_VERSION status {rsp[0]:#04x}")
    return rsp[1], rsp[2], rsp[3]


def i2c_scan(t: LinuxTarget, bus: int, span: tuple[int, int] | None = None,
             wake: bool = False) -> set[int]:
    """i2cdetect -r (read-byte probe, no quick-write), optionally limited to
    span=(first, last); UU counts as present. wake=True runs a throwaway
    probe first in the same command, for parts that NACK while asleep."""
    cmd = f"i2cdetect -y -r {bus}" + (f" {span[0]:#04x} {span[1]:#04x}" if span else "")
    out = t.run(f"{cmd} >/dev/null 2>&1; {cmd}" if wake else cmd).stdout
    found: set[int] = set()
    for ln in out.splitlines():
        m = re.match(r"([0-7]0): ", ln)
        if not m:
            continue
        base = int(m[1], 16)
        # Fixed 3-char columns: blanks pad the addresses outside the probed
        # range, so a whitespace split would shift every cell after them.
        for i in range(16):
            cell = ln[4 + 3 * i:6 + 3 * i]
            if cell == "UU" or re.fullmatch(r"[0-9a-f]{2}", cell):
                found.add(base + i)
    return found


def act88760_gpio4_held(t: LinuxTarget, bus: int) -> bool:
    """True while GD32_NRST is still held by the OTP GPIO4 default (0x88):
    the GD32 has not been released yet, by U-Boot or by us."""
    return i2c_get(t, bus, ACT88760_ADDR, ACT88760_GPIO_REG) == ACT88760_GPIO4_OTP_DEFAULT


def act88760_gpio4_release(t: LinuxTarget, bus: int) -> None:
    """Volatile workaround (lost at power-off): release GD32_NRST. Only when
    reg 0x10 still holds the OTP default -- never writes EEPROM/OTP."""
    if not act88760_gpio4_held(t, bus):
        raise BenchError(f"ACT88760 reg {ACT88760_GPIO_REG:#04x} is not {ACT88760_GPIO4_OTP_DEFAULT:#04x}; refusing to write")
    i2c_set(t, bus, ACT88760_ADDR, ACT88760_GPIO_REG, ACT88760_GPIO4_RELEASED)
    got = i2c_get(t, bus, ACT88760_ADDR, ACT88760_GPIO_REG)
    if got != ACT88760_GPIO4_RELEASED:
        raise BenchError(f"ACT88760 reg {ACT88760_GPIO_REG:#04x} reads {got:#04x} after writing {ACT88760_GPIO4_RELEASED:#04x}")


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
        # Parts that sleep NACK the first access after idle and ACK one made
        # right after it: the OPTIGA Trust M at 0x30 misses a plain scan and
        # falls back asleep between separate SSH commands (bench, E1M-V2M103
        # 2026W38-0001). Re-probe only the expected-but-silent addresses, each
        # as wake-then-scan inside one command.
        missing = {a for a in missing if a not in i2c_scan(t, bus, (a, a), wake=True)}
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


# --- DX-M1 NPU (V2M-only): sysfs GPIO + read-only status ---------------------------------
# The flash orchestration is provision/dxm1.py; these are the primitives it and the
# census share.

# the DEEPX 0.75 V rail; never a UART-mux/reset line -- gpiolib reconfigures
# a pin on read and would kill it. Enforced at the single sysfs choke point
# (_sysfs_gpio_dir) as well as steps.py's own bench.yaml validation.
DXM1_REFUSED_GPIO_LINES = {52: "P64", 53: "P65"}
# The DX-M1 is the only endpoint behind the root port, at bus 01.
DXM1_PCIE_DEVICE = "/sys/bus/pci/devices/0000:01:00.0/device"
DXM1_PCIE_FW_RUNNING = "0x0000"   # the firmware booted from the SPI-NAND
DXM1_PCIE_ROM_BOOT = "0x0001"     # the ROM's own PCIe-boot endpoint: no firmware on the NAND
# pinned to the real `dxrt-cli -s` line " * FW version          : v2.4.0" (fixture
# tests/scripts/fixtures/provision/dxrt-cli-s.txt); the "RT Driver version" and
# "PCIe Driver version" lines beside it must never match
_FW_LINE_RE = re.compile(r"(?m)^[ \t*]*FW version[ \t]*:[ \t]*v?(\d+\.\d+\.\d+[\w.+-]*)")


def _sysfs_gpio_line_name(line: int) -> str:
    """RZ/V2N pinctrl naming convention: within-chip `line` = port*8+pin ->
    "P<port><pin>", port 0-9 as a digit, port >=10 as a letter (A=10, ...) --
    e.g. line 61 -> "P75", line 86 -> "PA6". Matches the DT line names this
    board's pinctrl driver pre-exports some lines under in sysfs (see
    `_sysfs_gpio_dir`)."""
    port, pin = divmod(line, 8)
    p = str(port) if port < 10 else chr(ord("A") + port - 10)
    return f"P{p}{pin}"


def _pinctrl_chip_base(t: LinuxTarget, label: str) -> int:
    """Global sysfs GPIO base for the gpiochip whose /sys/class/gpio label
    matches `label` (e.g. "10410000.pinctrl") -- read from the live chip
    list rather than trusting a fixed base number, which shifts across
    kernel/DT revisions."""
    out = t.run('for d in /sys/class/gpio/gpiochip*; do '
                'echo "$d $(cat "$d/label")"; done', check=False).stdout
    for ln in out.splitlines():
        parts = ln.split(None, 1)
        if len(parts) == 2 and parts[1].strip() == label:
            return int(t.run(f"cat {parts[0]}/base").stdout.strip())
    raise BenchError(f"no /sys/class/gpio/gpiochip* with label {label!r} found")


def _sysfs_gpio_dir(t: LinuxTarget, label: str, line: int) -> str:
    """Resolve the sysfs dir for within-chip `line` on the gpiochip labelled
    `label`, exporting it if needed. This is the single choke point every
    DX-M1 GPIO helper goes through, so the DEEPX-rail refusal
    (`DXM1_REFUSED_GPIO_LINES`) is enforced right here, first -- before
    `_pinctrl_chip_base` or any `t.run` at all -- not just at steps.py's
    bench.yaml validation. Some lines on this board are already exported by
    the pinctrl driver under their DT name (e.g. "/sys/class/gpio/P75")
    rather than the numeric "gpio<N>"; if so, verify that named entry's
    `device` symlink actually resolves under THIS chip's own device (not a
    same-named line some other chip happens to export) before trusting it.
    A sysfs GPIO holds direction+value without a background process, so
    unlike gpioset this needs no PID bookkeeping."""
    if line in DXM1_REFUSED_GPIO_LINES:
        raise BenchError(f"gpio line {line} is {DXM1_REFUSED_GPIO_LINES[line]} "
                          "(the DEEPX 0.75 V rail): refusing to touch it")
    base = _pinctrl_chip_base(t, label)
    num = base + line
    numeric = f"/sys/class/gpio/gpio{num}"
    named = f"/sys/class/gpio/{_sysfs_gpio_line_name(line)}"
    chip_entry = f"/sys/class/gpio/gpiochip{base}"

    def named_exists_and_belongs_to_chip() -> bool:
        if t.run(f"test -e {named}/value", check=False).rc != 0:
            return False
        chip_dev = t.run(f"readlink -f {chip_entry}/device", check=False).stdout.strip()
        named_dev = t.run(f"readlink -f {named}/device", check=False).stdout.strip()
        return bool(chip_dev) and (named_dev == chip_dev or named_dev.startswith(chip_dev + "/"))

    if t.run(f"test -e {numeric}/value", check=False).rc == 0:
        return numeric
    if named_exists_and_belongs_to_chip():
        return named
    r = t.run(f"echo {num} > /sys/class/gpio/export", check=False)
    if r.rc != 0 and "busy" not in r.stderr.lower():
        raise BenchError(f"could not export sysfs gpio {num} ({label} line {line}): "
                          f"{r.stderr.strip()[-200:]}")
    if t.run(f"test -e {numeric}/value", check=False).rc == 0:
        return numeric
    if named_exists_and_belongs_to_chip():
        return named
    raise BenchError(f"gpio {num} ({label} line {line}) exported but neither "
                     f"{numeric} nor {named} appeared")


def dxm1_reset_pulse(t: LinuxTarget, label: str, reset_line: int, hold_s: float = 0.5) -> None:
    """Pulse PA6 (active-low M1_RESET) low for `hold_s` seconds then drive it high
    again, in one ssh round trip so the hold is timed by the board's own `sleep`,
    not host-to-board latency. `direction`'s "high"/"low" values set direction and
    value atomically (no separate value write to glitch on). Chained with `&&` so a
    failed `echo low` short-circuits instead of silently proceeding straight to the
    final `echo high`. DX-M1 PORES_N has an internal pull-up, so the final
    driven-high level is an active drive that happens to match the pin's idle
    (deasserted) state, not a "release" of the line."""
    d = _sysfs_gpio_dir(t, label, reset_line)
    t.run(f"echo low > {d}/direction && sleep {hold_s:g} && echo high > {d}/direction")


def dxm1_drive_high(t: LinuxTarget, label: str, line: int) -> str:
    """Export `line` and drive it high (a sysfs direction write holds on its own)."""
    d = _sysfs_gpio_dir(t, label, line)
    t.run(f"echo high > {d}/direction")
    return d


def dxm1_pcie_device(t: LinuxTarget) -> str | None:
    """The DX-M1's PCI device id ("0x0000" firmware running, "0x0001" ROM PCIe boot),
    None when no endpoint is enumerated at 0000:01:00.0. Read-only."""
    r = t.run(f"cat {DXM1_PCIE_DEVICE}", check=False)
    out = r.stdout.strip().lower()
    return out if r.rc == 0 and re.fullmatch(r"0x[0-9a-f]{4}", out) else None


def parse_dxm1_fw_version(text: str) -> str | None:
    """The firmware version from the `FW version : vX.Y.Z` line of `dxrt-cli -s`, without
    the leading "v"; None when that line is absent (driver / runtime versions never count)."""
    m = _FW_LINE_RE.search(text)
    return m[1] if m else None


def dxm1_fw_version(t: LinuxTarget) -> str | None:
    """`dxrt-cli -s` firmware version, None when the tool is absent or prints none."""
    r = t.run("dxrt-cli -s", timeout=60.0, check=False)
    return parse_dxm1_fw_version(r.stdout) if r.rc == 0 else None


# --- census ----------------------------------------------------------------------------

def cid_identity(raw: str) -> str:
    """The comparable part of a 128-bit eMMC CID, as 30 lower-case hex chars.

    Two sources must agree: bootstrap's EM_DCID parse (scif_writer.parse_cid) and the
    sysfs `cid`. The writer does print the CRC field, but parse_cid still rebuilds the
    register (reserved bits 119:114 forced to 0, end bit forced to 1), while the kernel
    host drivers do not agree on the last byte (SDHCI returns an R2 response without the
    CRC7 + end-bit byte). So byte 15 is not compared and the reserved bits of byte 1 are
    masked; MID, CBX, OID, PNM, PRV, PSN and MDT are."""
    raw = "".join(raw.split()).lower()
    if not re.fullmatch(r"[0-9a-f]{32}", raw):
        raise ValueError(f"CID must be 32 hex chars, got {raw!r}")
    return raw[:2] + f"{int(raw[2:4], 16) & 0x03:02x}" + raw[4:30]


def read_emmc_cid(t: LinuxTarget) -> str:
    """Raw sysfs CID of the eMMC (found by sysfs type, as census does)."""
    name = resolve_emmc(t).rsplit("/", 1)[-1]
    return t.run(f"cat /sys/block/{name}/device/cid").stdout.strip()


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


NET_IF_RE = re.compile(r"(?:end|eth)\d+")   # the Renesas gbeth ports are end0/end1


def net_ifaces(t) -> list[str]:
    return sorted(n for n in t.run("ls /sys/class/net", check=False).stdout.split()
                  if NET_IF_RE.fullmatch(n))


def net_carrier(t, name: str) -> bool:
    """True when the PHY reports link (an administratively-down port reads as no carrier)."""
    r = t.run(f"cat /sys/class/net/{name}/carrier", check=False)
    return r.rc == 0 and r.stdout.strip() == "1"


def census(t: LinuxTarget, i2c_bus: dict[str, int], sizes: dict[str, int] | None = None,
           emmc: str | None = None, dxm1_present: bool = True) -> tuple[dict[str, str], list[str]]:
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
        compat = t.run(r"tr '\0' ' ' < /proc/device-tree/compatible", check=False).stdout.strip()
        model = t.run(r"tr -d '\0' < /proc/device-tree/model", check=False).stdout.strip()
        if compat:
            facts["dtb_name"] = f"{model} (compatible {compat})" if model else compat

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

    def dxm1():
        if not dxm1_present:
            return
        dev = dxm1_pcie_device(t)
        facts["dxm1_pcie_device"] = dev or "absent"
        if dev == DXM1_PCIE_FW_RUNNING:
            ver = dxm1_fw_version(t)
            if ver:
                facts["dxm1_fw_version"] = ver
            else:
                notes.append("dxm1_fw_version: dxrt-cli -s printed no firmware version")

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
        names = net_ifaces(t)
        if not names:
            notes.append("network: no end*/eth* interface")
        # The ledger catalogue keys are eth0_*/eth1_* (by port index); the
        # interface names on the unit are end0/end1. Carrier, speed and the
        # link-partner advertisement are folded into the one *_link value.
        for i, n in enumerate(names[:2]):
            r = t.run(f"cat /sys/class/net/{n}/address /sys/class/net/{n}/operstate", check=False)
            lines = r.stdout.split()
            if r.rc != 0 or len(lines) != 2:
                notes.append(f"{n}: address/operstate unreadable")
                continue
            mac, state = lines
            spd = t.run(f"cat /sys/class/net/{n}/speed", check=False).stdout.strip()
            lp = re.search(r"Link partner advertised link modes:\s*(.+)",
                           t.run(f"ethtool {n} 2>/dev/null", check=False).stdout)
            carrier = "1" if net_carrier(t, n) else "0"
            speed = spd if spd.isdigit() else "unknown"
            anlpar = lp[1].strip() if lp else "unknown"
            facts[f"eth{i}_mac"] = mac
            facts[f"eth{i}_link"] = f"{state} ({n}) carrier={carrier} speed={speed} anlpar={anlpar}"

    for name, fn in (("soc", soc), ("cpu_mem", cpu_mem), ("storage", storage), ("xspi", xspi),
                     ("dxm1", dxm1), ("identity", identity), ("power", power), ("clocks_rtc", clocks_rtc),
                     ("network", network)):
        group(name, fn)
    return facts, notes
