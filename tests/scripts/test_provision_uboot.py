# SPDX-License-Identifier: Apache-2.0
"""scripts/provision/uboot.py against sanitised golden transcripts and an
XMODEM / loadx / gzwrite receiver simulator. No hardware."""

from __future__ import annotations

import gzip
import os
import re
import zlib
from pathlib import Path

import pytest
from provision import uboot
from provision.bench import BenchError, Console

from .provision_fakes import FakeConsole, FakePower

FIX = Path(__file__).parent / "fixtures" / "provision"
COLD_XSPI = (
    FIX / "uboot_cold_boot_xspi.log"
).read_bytes()  # BL2 -> U-Boot countdown, raw (NULs, CR CR LF)
CHAIN_RAIL = (
    FIX / "uboot_chain_rail_pg.log"
).read_bytes()  # chained U-Boot with the DEEPX rail lines + prompt


# ---------------------------------------------------------------- prompt catch + parsing


def test_cold_to_prompt_golden_transcript():
    c = FakeConsole([(None, "stale => junk"), (r"^ $", "\r\n=> ")])
    p = FakePower(on_hook=lambda: c.feed(COLD_XSPI))
    text = uboot.cold_to_prompt(c, p, off_s=0)
    assert p.events == ["off", "on"]
    assert c.written == [" "]
    assert text.rstrip().endswith("=>")

    assert uboot.parse_bl2(text) == {"bl2_version": "v2.10.5(release):alp-bench"}
    assert uboot.parse_bl31(text) == {"bl31_version": "v2.10.5(release):alp-bench"}
    assert uboot.parse_sys_lsi(text) == {
        "soc_sys_lsi_mode": "0x3c06",
        "soc_lsi_devid": "0x1867d447",
        "soc_prr": "0x0",
    }
    assert (
        uboot.parse_uboot_version(text)
        == "U-Boot 2024.07-alp+ (Sep 23 2026 - 21:30:30 +0200)"
    )
    assert uboot.parse_dram_banner(text) == 3994  # "3.9 GiB"
    assert not uboot.has_rail_pg(text)
    assert uboot.alp_lines(text) == ["ALP: no valid SoM manifest in EEPROM"]
    assert uboot.bl2_errors(text) == []  # "Error: ethernet..." is after U-Boot, not BL2


def test_rail_pg_golden_transcript():
    text = CHAIN_RAIL.decode()
    assert uboot.has_rail_pg(text)
    assert uboot.alp_lines(text) == [
        "ALP: no valid SoM manifest in EEPROM",
        "ALP: DA9292 programmed CTRL_01=0x01 VOUT_CH2=0x96/0x96",
        uboot.RAIL_PG,
    ]
    assert uboot.parse_bl2(text) == {} and uboot.parse_bl31(text) == {}


@pytest.mark.parametrize(
    "banner,mib",
    [
        ("DRAM:  8 GiB", 8192),
        ("DRAM:  3.9 GiB", 3994),
        ("DRAM:  512 MiB", 512),
        ("CPU: x", None),
    ],
)
def test_parse_dram_banner(banner, mib):
    assert uboot.parse_dram_banner(f"U-Boot 2024.07\r\n{banner}\r\n") == mib


def test_bl2_errors_only_before_uboot():
    text = (
        "NOTICE:  BL2: v2.10.5(release):x\r\n"
        "ERROR:   BL2: DDR training retry 1\r\n"
        "PANIC at PC : 0x0000000000012345\r\n"
        "U-Boot 2024.07-alp+ (x)\r\n"
        "Error: ethernet@15c30000 No valid MAC address found.\r\n"
    )
    assert uboot.bl2_errors(text) == [
        "ERROR:   BL2: DDR training retry 1",
        "PANIC at PC : 0x0000000000012345",
    ]


def test_stop_autoboot_times_out_without_countdown():
    with pytest.raises(BenchError):
        uboot.stop_autoboot(FakeConsole([(None, "Starting kernel ...")]), timeout=0.05)


# ---------------------------------------------------------------- run / md.l


def test_run_strips_echo_and_prompt():
    c = FakeConsole(
        [
            (
                r"^printenv bootcmd\r$",
                "printenv bootcmd\r\nbootcmd=run bootcmd_check\r\n=> ",
            )
        ]
    )
    assert uboot.run(c, "printenv bootcmd") == "bootcmd=run bootcmd_check"


def test_md_l_reads_words_in_sequence():
    out = (
        "md.l 0x10430000 0x6\r\n"
        "10430000: 00003c06 1867d447 00000000 deadbeef    .<..G.g.........\r\n"
        "10430010: 00000001 00000002                      ........\r\n=> "
    )
    c = FakeConsole([(r"^md\.l 0x10430000 0x6\r$", out)])
    assert uboot.md_l(c, 0x10430000, 6) == [0x3C06, 0x1867D447, 0, 0xDEADBEEF, 1, 2]


def test_md_l_rejects_gaps_short_reads_and_misalignment():
    gap = "10430000: 00000001\r\n10430008: 00000002\r\n=> "
    with pytest.raises(BenchError, match="out of sequence"):
        uboot.md_l(FakeConsole([(r"md\.l", gap)]), 0x10430000, 2)
    with pytest.raises(BenchError, match="wanted 4 words"):
        uboot.md_l(
            FakeConsole([(r"md\.l", "10430000: 00000001\r\n=> ")]), 0x10430000, 4
        )
    with pytest.raises(ValueError):
        uboot.md_l(FakeConsole([]), 0x10430002)


# ---------------------------------------------------------------- XMODEM / loadx sim


class LoadxSim(Console):
    """U-Boot loadx + gzwrite + go, byte-level. Verifies every XMODEM block."""

    def __init__(
        self, nak_first=False, stray_c=0, cancel=False, bad_crc=False, after_go=b""
    ):
        super().__init__()
        self.out = bytearray()
        self.rx = bytearray()
        self.receiving = False
        self.nak_first, self.stray_c, self.cancel, self.bad_crc = (
            nak_first,
            stray_c,
            cancel,
            bad_crc,
        )
        self.after_go = after_go
        self.blocks = 0
        self.naks = 0
        self.emmc = bytearray()
        self.lines: list[str] = []

    def _read_raw(self, timeout):
        data, self.out = bytes(self.out), bytearray()
        return data

    def _write_raw(self, data: bytes):
        if self.receiving:
            return self._xmodem(data)
        line = data.decode().rstrip("\r")
        self.lines.append(line)
        self.out += data + b"\n"  # echo
        if m := re.match(r"loadx (0x[0-9a-f]+)$", line):
            self.rx = bytearray()
            self.blocks = 0
            self.receiving = True
            self.out += f"## Ready for binary (xmodem) download to {m.group(1)} at 115200 bps...\r\n".encode()
            self.out += b"CC"
        elif m := re.match(
            r"gzwrite mmc (\d+) 0x[0-9a-f]+ (0x[0-9a-f]+) 100000 (0x[0-9a-f]+)$", line
        ):
            piece = gzip.decompress(bytes(self.rx[: int(m.group(2), 16)]))
            off = int(m.group(3), 16)
            self.emmc[off : off + len(piece)] = piece
            crc = zlib.crc32(piece) ^ (1 if self.bad_crc else 0)
            self.out += f"\r\n\t{len(piece)} bytes, crc 0x{crc:08x}\r\n=> ".encode()
        elif "go 0x" in line:
            self.out += self.after_go
        elif line == " ":
            pass  # autoboot key

    def _xmodem(self, pkt: bytes):
        if pkt == b"\x04":
            self.receiving = False
            self.out += b"\x06"
            self.out += f"\r\n## Total Size      = 0x{len(self.rx):08x} = {len(self.rx)} Bytes\r\n=> ".encode()
            return
        assert len(pkt) == 1029 and pkt[0] == 0x02
        blk = (self.blocks + 1) & 0xFF
        assert pkt[1] == blk and pkt[2] == 0xFF - blk
        assert int.from_bytes(pkt[-2:], "big") == uboot.crc16_xmodem(pkt[3:-2])
        if self.cancel:
            self.out += b"\x18\x18"
            return
        if self.nak_first and self.naks == 0:
            self.naks += 1
            self.out += b"\x15"
            return
        self.blocks += 1
        self.rx += pkt[3:-2]
        self.out += b"C" * self.stray_c + b"\x06"


def test_crc16_xmodem_check_value():
    assert uboot.crc16_xmodem(b"123456789") == 0x31C3


@pytest.mark.parametrize("kw", [{}, {"nak_first": True}, {"stray_c": 3}])
def test_loadx_roundtrip(kw):
    data = os.urandom(2500)
    sim = LoadxSim(**kw)
    size = uboot.loadx(sim, data, 0x48000000)
    assert size == 3072  # padded to 3 x 1 KiB
    assert bytes(sim.rx) == data + b"\x1a" * (3072 - 2500)
    assert sim.lines == ["loadx 0x48000000"]


def test_loadx_cancel_is_bench_error():
    with pytest.raises(BenchError, match="cancelled at block 1"):
        uboot.loadx(LoadxSim(cancel=True), b"x" * 10, 0x48000000)


def test_xmodem_unacked_block_gives_up(monkeypatch):
    class Deaf(LoadxSim):
        def _xmodem(self, pkt):
            pass

    monkeypatch.setattr(uboot, "_XM_BLOCK_TIMEOUT", 0.01)
    sim = Deaf()
    with pytest.raises(BenchError, match="block 1 not acknowledged after 10"):
        uboot.loadx(sim, b"x", 0x48000000, timeout=1)


def test_chain_load_holds_the_chained_prompt():
    sim = LoadxSim(
        after_go=b"## Starting application at 0x70000000 ...\r\n\r\n" + CHAIN_RAIL
    )
    text = uboot.chain_load(sim, b"\xaa" * 4096, 0x70000000)
    assert sim.lines == [
        "loadx 0x70000000",
        "dcache flush; dcache off; icache off; go 0x70000000",
        " ",
    ]
    assert uboot.has_rail_pg(text)
    assert uboot.parse_uboot_version(text).startswith(
        "U-Boot 2024.07-alp+ (Sep 24 2026"
    )


def _wic(tmp_path: Path, data: bytes) -> Path:
    p = tmp_path / "img.wic.gz"
    p.write_bytes(gzip.compress(data))
    return p


def test_loadx_gzwrite_reassembles_image(tmp_path):
    data = os.urandom(1536) + bytes(2048) + os.urandom(512)  # 4096 bytes
    sim = LoadxSim()
    uboot.loadx_gzwrite(sim, _wic(tmp_path, data), 0, 0x48000000, chunk=1536)
    assert bytes(sim.emmc) == data
    gz = [ln for ln in sim.lines if ln.startswith("gzwrite")]
    assert [ln.split()[-1] for ln in gz] == ["0x0", "0x600", "0xc00"]
    assert all(ln.startswith("gzwrite mmc 0 0x48000000 ") for ln in gz)


def test_loadx_gzwrite_crc_mismatch_fails(tmp_path):
    with pytest.raises(BenchError, match="gzwrite at offset 0x0"):
        uboot.loadx_gzwrite(
            LoadxSim(bad_crc=True),
            _wic(tmp_path, bytes(1024)),
            0,
            0x48000000,
            chunk=1024,
        )


def test_loadx_gzwrite_input_checks(tmp_path):
    with pytest.raises(ValueError, match="multiple of 512"):
        uboot.loadx_gzwrite(
            LoadxSim(), _wic(tmp_path, bytes(1024)), 0, 0x48000000, chunk=1000
        )
    with pytest.raises(ValueError, match="image size"):
        uboot.loadx_gzwrite(
            LoadxSim(), _wic(tmp_path, bytes(700)), 0, 0x48000000, chunk=512
        )
