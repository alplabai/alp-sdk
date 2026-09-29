# SPDX-License-Identifier: Apache-2.0
"""Drive the RZ/V2N boot ROM's SCIF download mode and the Renesas Flash Writer.

The bootstrap step of V2N provisioning (docs/provisioning-v2n.md,
``bootstrap``) uses this module to put a *transient* eMMC-boot BL2 and a FIP
into eMMC boot partition 1 before the unit has any bootloader of its own:

1. With DSW1 on SCIF download, the boot ROM prints ``ROM_BANNER`` and waits
   for an S-record image. ``load_writer`` streams the Flash Writer ``.mot``
   to it raw, in 4 KiB chunks, and waits for the writer's ``>`` prompt.
2. ``em_w`` runs the writer's ``EM_W`` dialog: partition area, start sector
   (hex), program start address (hex), then the image as S-records.
3. ``em_secsd`` runs ``EM_SECSD`` to set one EXT_CSD byte and checks the
   value the writer reads back.
4. ``em_dcid`` runs ``EM_DCID`` and parses the CID the writer prints.

Every prompt and error string is a module constant, copied from the
Flash Writer's own output. The ROM banner has not been checked on this
bench yet (see ``ROM_BANNER``). All console traffic goes through
``provision.bench.Console``; nothing here loops on console input itself.
"""

from __future__ import annotations

import re
from pathlib import Path

from provision.bench import BenchError, Console
from provision.gates import BL2_MMC_SECTOR, FIP_SECTOR  # noqa: F401  (re-exported)

# --- prompts (module constants; the ROM banner is still to be confirmed on the bench) ---
ROM_BANNER = r"SCI Download mode"
WRITER_PROMPT = r"\n>"
AREA_PROMPT = r"Select area\(0-2\)>"
SECTOR_PROMPT = r"Please Input Start Address in sector :"
PROGRAM_START_PROMPT = r"Please Input Program Start Address :"
SEND_PROMPT = r"please send ! \('\.' & CR stop load\)"
EM_W_DONE = r"EM_W Complete!"
EXT_CSD_INDEX_PROMPT = r"Please Input EXT_CSD Index\(H'00 - H'1FF\) :"
EXT_CSD_VALUE_PROMPT = r"Please Input Value\(H'00 - H'FF\) :"
EXT_CSD_ECHO = r"EXT_CSD\[(?P<idx>[0-9A-Fa-f]{2})\] = 0x(?P<val>[0-9A-Fa-f]{2})"
YES_NO = r"\(y/n\)"
CID_BLOCK = r"(?s)\[CID Field Data\](?P<body>.*?)\n>"
# Every failure line the writer prints in these dialogs. Its input loops
# re-prompt on bad input forever, so a Syntax/Param error must stop us.
WRITER_ERROR = (r"[^\r\n]*(?:ERROR!|ERR!|\bERR\b|FAIL|Syntax Error|Pa[lr]am Error"
                r"|Size Over|Boundary Error|Unwritable Index|CMD8 error)[^\r\n]*")

BOOT1_AREA = 1
EXT_CSD_WRITES = ((177, 0x02), (179, 0x08))   # BOOT_BUS_CONDITIONS, PARTITION_CONFIG=boot1

CHUNK = 4096            # bytes per write while streaming an image
SREC_DATA_LEN = 32      # data bytes per S3 record
PROMPT_TIMEOUT = 10.0   # one dialog question


def bin_to_srec(data: bytes, load_addr: int) -> bytes:
    """Return ``data`` as S3 records starting at ``load_addr``, then an S7, CRLF-terminated."""
    if not data:
        raise ValueError("bin_to_srec: empty image")
    if load_addr < 0 or load_addr + len(data) > 1 << 32:
        raise ValueError(f"bin_to_srec: image does not fit 32-bit address space at {load_addr:#x}")

    def rec(kind: str, addr: int, payload: bytes) -> bytes:
        body = bytes([4 + len(payload) + 1]) + addr.to_bytes(4, "big") + payload
        return f"{kind}{body.hex().upper()}{~sum(body) & 0xFF:02X}\r\n".encode("ascii")

    out = [rec("S3", load_addr + i, data[i:i + SREC_DATA_LEN])
           for i in range(0, len(data), SREC_DATA_LEN)]
    out.append(rec("S7", load_addr, b""))
    return b"".join(out)


def _stream(console: Console, blob: bytes) -> None:
    for i in range(0, len(blob), CHUNK):
        console.write(blob[i:i + CHUNK])


def _ask(console: Console, prompt: str, what: str, timeout: float = PROMPT_TIMEOUT):
    """Wait for ``prompt``; a writer error line first raises BenchError naming ``what``."""
    # Earliest match wins, so an error line printed before a re-prompt is caught.
    key, m = console.expect_any({"err": WRITER_ERROR, "ok": prompt}, timeout)
    if key == "err":
        raise BenchError(f"{what}: Flash Writer reported {m.group(0).strip()!r}")
    return m


def load_writer(console: Console, mot: Path, timeout: float = 120.0) -> None:
    """Wait for the boot-ROM banner, stream the Flash Writer ``.mot``, wait for its prompt."""
    image = Path(mot).read_bytes()
    console.expect(ROM_BANNER, timeout)
    _stream(console, image)
    console.expect(WRITER_PROMPT, timeout)


def em_w(console: Console, area: int, start_sector: int, program_start: int,
         image: bytes, timeout: float = 600.0) -> None:
    """Write ``image`` to eMMC ``area`` at ``start_sector`` with the writer's EM_W command."""
    if area not in (0, 1, 2):
        raise ValueError(f"em_w: area must be 0, 1 or 2, got {area}")
    if start_sector < 0 or program_start < 0:
        raise ValueError("em_w: start_sector and program_start must be non-negative")
    srec = bin_to_srec(image, program_start)
    console.send_line("EM_W")
    _ask(console, AREA_PROMPT, "EM_W area")
    console.send_line(str(area))
    _ask(console, SECTOR_PROMPT, "EM_W area")
    console.send_line(f"{start_sector:X}")
    _ask(console, PROGRAM_START_PROMPT, "EM_W start sector")
    console.send_line(f"{program_start:X}")
    _ask(console, SEND_PROMPT, "EM_W program start")
    _stream(console, srec)
    _ask(console, EM_W_DONE, "EM_W write", timeout)
    console.expect(WRITER_PROMPT, PROMPT_TIMEOUT)


def em_secsd(console: Console, index: int, value: int) -> None:
    """Set EXT_CSD[index] = value with EM_SECSD; raise BenchError unless the read-back matches."""
    if not 0 <= index <= 0x1FF or not 0 <= value <= 0xFF:
        raise ValueError(f"em_secsd: index {index} / value {value} out of range")
    console.send_line("EM_SECSD")
    _ask(console, EXT_CSD_INDEX_PROMPT, "EM_SECSD")
    console.send_line(f"{index:X}")
    _ask(console, EXT_CSD_VALUE_PROMPT, f"EM_SECSD index {index:#x}")
    console.send_line(f"{value:X}")
    # Builds with EXTCSD_PROTECT ask "(y/n)" (twice) for protected indexes;
    # the writer reads a single key, no CR.
    for _ in range(3):
        key, m = console.expect_any(
            {"err": WRITER_ERROR, "yn": YES_NO, "echo": EXT_CSD_ECHO}, PROMPT_TIMEOUT)
        if key == "yn":
            console.write(b"y")
            continue
        if key == "err":
            raise BenchError(f"EM_SECSD index {index:#x}: Flash Writer reported {m.group(0).strip()!r}")
        break
    else:
        raise BenchError("EM_SECSD: more (y/n) questions than expected")
    idx, val = int(m.group("idx"), 16), int(m.group("val"), 16)
    if idx != index & 0xFF or val != value:
        raise BenchError(f"EM_SECSD: EXT_CSD[{index}] reads back {val:#04x} "
                         f"(index echo {idx:#04x}), wanted {value:#04x}")
    console.expect(WRITER_PROMPT, PROMPT_TIMEOUT)


# CID fields as the writer prints them: "[127:120]  MID  0x15" (JEDEC eMMC CID bit ranges).
_CID_FIELD = re.compile(r"\[\s*(\d+):\s*(\d+)\]\s+(MID|CBX|OID|PNM|PRV|PSN|MDT|CRC)\s+0x([0-9A-Fa-f]+)")
_CID_LSB = {"MID": 120, "CBX": 112, "OID": 104, "PNM": 56, "PRV": 48, "PSN": 16, "MDT": 8, "CRC": 1}


def parse_cid(text: str) -> dict[str, str]:
    """Parse the writer's ``[CID Field Data]`` block into the ``emmc_cid_*`` ledger keys.

    The writer prints fields, not the raw register, so ``emmc_cid_raw`` is
    rebuilt from them: the reserved bits 119:114 are 0 by JEDEC and bit 0 is
    always 1.
    """
    fields = {name: int(val, 16) for _, _, name, val in _CID_FIELD.findall(text)}
    missing = [f for f in _CID_LSB if f not in fields]
    if missing:
        raise BenchError(f"EM_DCID: CID fields missing from writer output: {', '.join(missing)}")
    raw = 1
    for name, lsb in _CID_LSB.items():
        raw |= fields[name] << lsb
    pnm = fields["PNM"].to_bytes(6, "big").decode("ascii", "replace").strip("\x00 ")
    return {
        "emmc_cid_raw": f"{raw:032x}",
        "emmc_cid_mid": f"{fields['MID']:#04x}",
        "emmc_cid_pnm": pnm,
        "emmc_cid_prv": f"{fields['PRV']:#04x}",
        "emmc_cid_psn": f"{fields['PSN']:#010x}",
        "emmc_cid_mdt": f"{fields['MDT']:#04x}",
    }


def em_dcid(console: Console) -> dict[str, str]:
    """Run EM_DCID and return the parsed CID (see ``parse_cid``)."""
    console.send_line("EM_DCID")
    m = _ask(console, CID_BLOCK, "EM_DCID")
    return parse_cid(m.group("body"))
