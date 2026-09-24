# SPDX-License-Identifier: Apache-2.0
"""U-Boot / BL2 console interaction for SoM provisioning.

Catching the prompt after a power cycle, running commands, ``md.l`` reads,
banner parsing (BL2 / BL31 / U-Boot / DRAM / ``ALP:`` lines / SYS_LSI), an
XMODEM-1K (CRC) sender for ``loadx``, chain-loading a RAM U-Boot, and the
slow ``loadx`` + ``gzwrite`` rootfs fallback. All console waiting goes
through ``Console.expect``.
"""

from __future__ import annotations

import gzip
import re
import time
import zlib
from pathlib import Path

from provision.bench import BenchError, Console, ExpectTimeout, Power
from provision.gates import RAIL_PG

PROMPT = r"=> "
AUTOBOOT = r"Hit any key to stop autoboot"

UBOOT_BANNER_RE = re.compile(r"^U-Boot 20\d\d\.\d\d.*$", re.MULTILINE)
DRAM_RE = re.compile(r"^DRAM:\s+([0-9.]+)\s*([KMG])iB", re.MULTILINE)
BL2_VERSION_RE = re.compile(r"BL2: (v\d+\.\d+\S*)")
BL31_VERSION_RE = re.compile(r"BL31: (v\d+\.\d+\S*)")
# TBD (bench-verify): the RZ/V2N BL2 boot-source line has not been captured
# on the bench yet; the key is simply omitted until this matches.
BL2_BOOT_SOURCE_RE = re.compile(
    r"BL2: (?:Boot device|Boot source|boot from)\s*[:=]?\s*(\S.*?)\s*$",
    re.MULTILINE | re.IGNORECASE,
)
SYS_LSI_RE = re.compile(r"BL2: SYS_LSI_(MODE|DEVID|PRR): 0[xX]([0-9A-Fa-f]+)")
_SYS_LSI_KEYS = {"MODE": "soc_sys_lsi_mode", "DEVID": "soc_lsi_devid", "PRR": "soc_prr"}
# Lines in the pre-U-Boot (BL2/BL31) output that mean the boot is not clean.
# ERROR/PANIC/Assertion per the contract; failed/retry catch DDR-training retries.
BL2_ERROR_RE = re.compile(r"ERROR|PANIC|Assertion|[Ff]ailed|[Rr]etry")
MD_LINE_RE = re.compile(r"^([0-9a-fA-F]{8,16}):((?: [0-9a-fA-F]{8})+)", re.MULTILINE)
LOADX_READY_RE = r"Ready for binary \(xmodem\) download"
LOADX_TOTAL_RE = r"## Total Size\s*=\s*0x([0-9a-fA-F]+)"
# TBD (bench-verify): lib/gunzip.c success line, "<n> bytes, crc 0x<8 hex>".
GZWRITE_DONE_RE = re.compile(r"(\d+) bytes, crc 0x([0-9a-fA-F]{8})")

# XMODEM control bytes, matched as decoded single characters.
_STX, _EOT = 0x02, 0x04
_XM_REPLY = {"ack": "\x06", "nak": "\x15", "can": "\x18", "c": "C"}
_XM_RETRIES = 10
_XM_BLOCK_TIMEOUT = 5.0


# --------------------------------------------------------------------------
# prompt / commands
# --------------------------------------------------------------------------


def stop_autoboot(console: Console, timeout: float = 30.0) -> str:
    """Wait for the autoboot countdown, send one key, wait for the prompt.
    Returns all text seen (the BL2/BL31/U-Boot banners)."""
    m = console.expect(AUTOBOOT, timeout)
    seen = m.string[: m.end()]
    console.write(b" ")
    p = console.expect(PROMPT, 10.0)
    return seen + p.string[: p.end()]


def cold_to_prompt(
    console: Console, power: Power, off_s: float = 3.0, timeout: float = 60.0
) -> str:
    """Cold power cycle and hold U-Boot at its prompt; returns the boot text."""
    console.drain()
    power.cycle(off_s)
    return stop_autoboot(console, timeout)


def run(console: Console, cmd: str, timeout: float = 10.0) -> str:
    """Run one U-Boot command; returns its output (echo and prompt removed)."""
    console.send_line(cmd)
    m = console.expect(PROMPT, timeout)
    out = m.string[: m.start()].lstrip("\r\n")
    out = out.removeprefix(cmd)
    return out.strip("\r\n")


def md_l(
    console: Console, addr: int, count: int = 1, timeout: float = 10.0
) -> list[int]:
    """``md.l addr count`` -> the `count` 32-bit words, address-checked."""
    if addr % 4:
        raise ValueError(f"md.l address {addr:#x} is not 4-byte aligned")
    out = run(console, f"md.l {addr:#x} {count:#x}", timeout)
    words: list[int] = []
    for m in MD_LINE_RE.finditer(out):
        if int(m.group(1), 16) != addr + 4 * len(words):
            raise BenchError(f"md.l output out of sequence at {m.group(1)}:\n{out}")
        words += [int(w, 16) for w in m.group(2).split()]
    if len(words) < count:
        raise BenchError(
            f"md.l {addr:#x}: wanted {count} words, got {len(words)}:\n{out}"
        )
    return words[:count]


# --------------------------------------------------------------------------
# banner parsing (pure)
# --------------------------------------------------------------------------


def parse_bl2(text: str) -> dict[str, str]:
    out = {}
    if m := BL2_VERSION_RE.search(text):
        out["bl2_version"] = m.group(1)
    if m := BL2_BOOT_SOURCE_RE.search(text):
        out["bl2_boot_source"] = m.group(1)
    return out


def parse_bl31(text: str) -> dict[str, str]:
    m = BL31_VERSION_RE.search(text)
    return {"bl31_version": m.group(1)} if m else {}


def parse_sys_lsi(text: str) -> dict[str, str]:
    """BL2's SYS_LSI_MODE / DEVID / PRR notices -> soc_* ledger keys, lower-case 0x hex."""
    return {_SYS_LSI_KEYS[k]: f"0x{int(v, 16):x}" for k, v in SYS_LSI_RE.findall(text)}


def parse_uboot_version(text: str) -> str | None:
    m = UBOOT_BANNER_RE.search(text)
    return m.group(0).strip() if m else None


def parse_dram_banner(text: str) -> int | None:
    """``DRAM:  8 GiB`` -> 8192 (MiB). U-Boot rounds to one decimal, so a
    4 GiB part whose usable window is 3.875 GiB prints ``3.9 GiB`` -> 3994."""
    m = DRAM_RE.search(text)
    if not m:
        return None
    return round(float(m.group(1)) * {"K": 1 / 1024, "M": 1, "G": 1024}[m.group(2)])


def has_rail_pg(text: str) -> bool:
    return RAIL_PG in text


def alp_lines(text: str) -> list[str]:
    """Every ``ALP: ...`` line U-Boot's board code printed."""
    return [ln.strip() for ln in re.findall(r"^ALP: .*$", text, re.MULTILINE)]


def bl2_errors(text: str) -> list[str]:
    """Error lines in the BL2/BL31 part of a boot log (text before U-Boot)."""
    m = UBOOT_BANNER_RE.search(text)
    pre = text[: m.start()] if m else text
    return [ln.strip() for ln in pre.splitlines() if BL2_ERROR_RE.search(ln)]


# --------------------------------------------------------------------------
# XMODEM-1K (CRC) sender, loadx, chain-load
# --------------------------------------------------------------------------


def crc16_xmodem(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) if crc & 0x8000 else (crc << 1)
            crc &= 0xFFFF
    return crc


def xmodem_send(console: Console, data: bytes, timeout: float = 60.0) -> None:
    """Sender side of XMODEM-CRC with 1 KiB (STX) blocks, 0x1A padding.

    Waits up to `timeout` for the receiver's 'C'; stray 'C's before a block's
    ACK are skipped; NAK or silence resends (10 tries); CAN aborts.
    """
    console.expect("C", timeout)
    blk = 1
    for off in range(0, len(data), 1024):
        chunk = data[off : off + 1024].ljust(1024, b"\x1a")
        pkt = (
            bytes([_STX, blk & 0xFF, 0xFF - (blk & 0xFF)])
            + chunk
            + crc16_xmodem(chunk).to_bytes(2, "big")
        )
        _xm_exchange(console, pkt, f"block {blk}")
        blk += 1
    _xm_exchange(console, bytes([_EOT]), "EOT")


def _xm_exchange(console: Console, pkt: bytes, what: str) -> None:
    for _ in range(_XM_RETRIES):
        console.write(pkt)
        deadline = time.monotonic() + _XM_BLOCK_TIMEOUT
        key = "timeout"
        while (left := deadline - time.monotonic()) > 0:
            try:
                key, _m = console.expect_any(_XM_REPLY, left)
            except ExpectTimeout:
                key = "timeout"
            if key != "c":  # stray 'C' from the receiver's start phase
                break
            key = "timeout"
        if key == "ack":
            return
        if key == "can":
            raise BenchError(f"XMODEM receiver cancelled at {what}")
    raise BenchError(f"XMODEM {what} not acknowledged after {_XM_RETRIES} tries")


def loadx(console: Console, data: bytes, addr: int, timeout: float = 60.0) -> int:
    """``loadx addr`` + XMODEM send; returns U-Boot's reported total size
    (the 1 KiB-padded length)."""
    console.send_line(f"loadx {addr:#x}")
    console.expect(LOADX_READY_RE, timeout)
    xmodem_send(console, data, timeout)
    size = int(console.expect(LOADX_TOTAL_RE, 30.0).group(1), 16)
    console.expect(PROMPT, 10.0)
    if size < len(data):
        raise BenchError(f"loadx received {size} bytes, sent {len(data)}")
    return size


def chain_load(console: Console, image: bytes, addr: int, timeout: float = 60.0) -> str:
    """Load a position-independent U-Boot into RAM at `addr`, jump to it and
    hold ITS prompt. Returns the chained U-Boot's banner text."""
    loadx(console, image, addr, timeout)
    console.send_line(f"dcache flush; dcache off; icache off; go {addr:#x}")
    return stop_autoboot(console, timeout)


def loadx_gzwrite(
    console: Console,
    wic_gz: Path,
    emmc_dev: int,
    load_addr: int,
    chunk: int,
    timeout: float = 600.0,
) -> None:
    """Fallback rootfs path over the U-Boot console (slow: ~11 KiB/s at 115200).

    The wic is decompressed host-side and split into `chunk`-byte pieces; each
    piece is re-gzipped (so each is a complete gzip stream), sent with
    ``loadx`` and written with ``gzwrite mmc <dev> <addr> <len> 100000 <offs>``.
    Every piece is verified against gzwrite's own byte count and CRC32.
    """
    if chunk <= 0 or chunk % 512:
        raise ValueError(f"chunk {chunk} must be a positive multiple of 512")
    off = 0
    with gzip.open(wic_gz, "rb") as f:
        while piece := f.read(chunk):
            if len(piece) % 512:
                raise ValueError(
                    f"{wic_gz}: image size is not a multiple of 512 (tail {len(piece)} bytes)"
                )
            gz = gzip.compress(piece, mtime=0)
            loadx(console, gz, load_addr)
            out = run(
                console,
                f"gzwrite mmc {emmc_dev} {load_addr:#x} {len(gz):#x} 100000 {off:#x}",
                timeout,
            )
            m = GZWRITE_DONE_RE.search(out)
            want_crc = zlib.crc32(piece)
            if (
                not m
                or int(m.group(1)) != len(piece)
                or int(m.group(2), 16) != want_crc
            ):
                raise BenchError(
                    f"gzwrite at offset {off:#x}: expected {len(piece)} bytes crc {want_crc:#010x}, got:\n{out}"
                )
            off += len(piece)
