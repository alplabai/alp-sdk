"""Tests for scripts/provision/scif_writer.py, driven by replayed Flash Writer
console transcripts. The transcript text is the writer's own output strings
(prompts, echoes, CID field layout); the CID values are synthetic."""

from __future__ import annotations

import pytest

from provision import scif_writer as sw
from provision.bench import BenchError, ExpectTimeout
from .provision_fakes import FakeConsole

PROMPT = "\r\n>"
AREA_MENU = (
    "EM_W\r\nEM_W Start --------------\r\n"
    "---------------------------------------------------------\r\n"
    "Please select,eMMC Partition Area.\r\n"
    " 0:User Partition Area   : 30535680 KBytes\r\n  eMMC Sector Cnt : H'0 - H'03A3DFFF\r\n"
    " 1:Boot Partition 1      : 32256 KBytes\r\n  eMMC Sector Cnt : H'0 - H'0000FBFF\r\n"
    " 2:Boot Partition 2      : 32256 KBytes\r\n  eMMC Sector Cnt : H'0 - H'0000FBFF\r\n"
    "---------------------------------------------------------\r\n"
    "  Select area(0-2)>"
)


def _stream_entries(blob: bytes, last_out: str) -> list[tuple[str | None, str]]:
    """One script entry per CHUNK-sized write; the last one emits `last_out`."""
    n = -(-len(blob) // sw.CHUNK)
    return [(r"(?s).", "")] * (n - 1) + [(r"(?s).", last_out)]


def _written_blob(console: FakeConsole, first: int) -> bytes:
    return "".join(console.written[first:]).encode("latin-1")


# --- bin_to_srec ---------------------------------------------------------

def test_bin_to_srec_records_checksums_and_terminator():
    data = bytes(range(40))
    lines = sw.bin_to_srec(data, 0x44000000).split(b"\r\n")
    assert lines[-1] == b""
    assert [l[:2] for l in lines[:-1]] == [b"S3", b"S3", b"S7"]
    assert lines[0].startswith(b"S325" + b"44000000")        # 4 addr + 32 data + 1 sum
    assert lines[1].startswith(b"S30D" + b"44000020")        # 4 addr + 8 data + 1 sum
    assert lines[2] == b"S70544000000B6"
    for line in lines[:-1]:
        body = bytes.fromhex(line[2:].decode())
        assert sum(body) & 0xFF == 0xFF                         # count..data + checksum
    payload = b"".join(bytes.fromhex(l[12:-2].decode()) for l in lines[:2])
    assert payload == data


@pytest.mark.parametrize("data,addr", [(b"", 0), (b"x", -1), (b"xx", 0xFFFFFFFF)])
def test_bin_to_srec_rejects_bad_input(data, addr):
    with pytest.raises(ValueError):
        sw.bin_to_srec(data, addr)


# --- load_writer -----------------------------------------------------------

def test_load_writer_waits_for_banner_streams_mot_and_waits_for_prompt(tmp_path):
    mot = tmp_path / "writer.mot"
    mot.write_bytes(b"S00F000068656C6C6F202020202000003C\r\n" * 300)   # > one chunk
    banner = "\r\nSCI Download mode (Normal SCI boot)\r\n-- Load Program to SRAM ---------------\r\n"
    writer = "\r\nFlash writer for RZ/V2N Series V0.90 Jan.12,2024\r\n Product Code : RZ/V2N" + PROMPT
    con = FakeConsole([(None, banner)] + _stream_entries(mot.read_bytes(), writer))
    sw.load_writer(con, mot, timeout=1)
    assert _written_blob(con, 0) == mot.read_bytes()
    assert all(len(w) <= sw.CHUNK for w in con.written)
    assert not con.script


def test_load_writer_times_out_without_banner(tmp_path):
    mot = tmp_path / "writer.mot"
    mot.write_bytes(b"S0030000FC\r\n")
    con = FakeConsole([(None, "garbage from a unit that is not in SCIF mode\r\n")])
    with pytest.raises(ExpectTimeout):
        sw.load_writer(con, mot, timeout=0.05)
    assert con.written == []    # nothing streamed at a unit that never asked for it


# --- EM_W ------------------------------------------------------------------

def _em_w_script(image: bytes, start: int, sector: int, done: str):
    srec = sw.bin_to_srec(image, start)
    return [
        (r"^EM_W\r$", AREA_MENU),
        (r"^1\r$", "1\r\n-- Boot Partition 1 Program -----------------------------\r\n"
                   "Please Input Start Address in sector :"),
        (rf"^{sector:X}\r$", f"{sector:X}\r\nPlease Input Program Start Address : "),
        (rf"^{start:X}\r$", f"{start:X}\r\nWork RAM (H'50000000-H'7FFFFFFF) Clear....\r\n"
                            "please send ! ('.' & CR stop load)\r\n"),
    ] + _stream_entries(srec, done)


def test_em_w_replayed_dialog_boot1():
    image = bytes(range(256)) * 40        # 10 KiB -> several S-record chunks
    done = "SAVE -FLASH.......\r\nEM_W Complete!" + PROMPT
    con = FakeConsole(_em_w_script(image, 0x44000000, sw.FIP_SECTOR, done))
    sw.em_w(con, sw.BOOT1_AREA, sw.FIP_SECTOR, 0x44000000, image, timeout=1)
    assert con.written[:4] == ["EM_W\r", "1\r", "300\r", "44000000\r"]
    assert _written_blob(con, 4) == sw.bin_to_srec(image, 0x44000000)
    assert not con.script


def test_em_w_write_error_raises():
    image = b"\x00" * 64
    con = FakeConsole(_em_w_script(image, 0x1000, sw.BL2_MMC_SECTOR,
                                   "SAVE -FLASH.......\r\nEM_W ERR" + PROMPT))
    with pytest.raises(BenchError, match="EM_W ERR"):
        sw.em_w(con, sw.BOOT1_AREA, sw.BL2_MMC_SECTOR, 0x1000, image, timeout=1)


def test_em_w_param_error_stops_instead_of_looping():
    con = FakeConsole([
        (r"^EM_W\r$", AREA_MENU),
        (r"^1\r$", "1\r\n-- Boot Partition 1 Program --\r\nPlease Input Start Address in sector :"),
        (r"^FFFFFF\r$", "FFFFFF\r\nParam Error\r\nPlease Input Start Address in sector :"),
    ])
    with pytest.raises(BenchError, match="Param Error"):
        sw.em_w(con, 1, 0xFFFFFF, 0x1000, b"\x00" * 16, timeout=1)


def test_em_w_rejects_bad_area():
    with pytest.raises(ValueError):
        sw.em_w(FakeConsole([]), 3, 1, 0x1000, b"x")


# --- EM_SECSD --------------------------------------------------------------

def _secsd_script(index: int, old: int):
    return [
        (r"^EM_SECSD\r$", "EM_SECSD\r\n  Please Input EXT_CSD Index(H'00 - H'1FF) :"),
        (rf"^{index:X}\r$", f"{index:X}\r\n  EXT_CSD[{index & 0xFF:02X}] = 0x{old:02X}\r\n"
                            "  Please Input Value(H'00 - H'FF) :"),
    ]


@pytest.mark.parametrize("index,value", sw.EXT_CSD_WRITES)
def test_em_secsd_sets_and_verifies(index, value):
    script = _secsd_script(index, 0x00)
    script.append((rf"^{value:X}\r$", f"{value:X}\r\n  EXT_CSD[{index:02X}] = 0x{value:02X}" + PROMPT))
    con = FakeConsole(script)
    sw.em_secsd(con, index, value)
    assert con.written == ["EM_SECSD\r", f"{index:X}\r", f"{value:X}\r"]
    assert not con.script


def test_em_secsd_answers_protect_warning_with_single_key():
    warn = "\r\n!! Warning !! This field contains the item of protection.\r\n              Change OK?(y/n)"
    script = _secsd_script(0xB3, 0x00)
    script += [(r"^8\r$", "8" + warn), (r"^y$", warn),
               (r"^y$", "\r\n  EXT_CSD[B3] = 0x08" + PROMPT)]
    con = FakeConsole(script)
    sw.em_secsd(con, 0xB3, 0x08)
    assert con.written[-2:] == ["y", "y"]


def test_em_secsd_readback_mismatch_raises():
    script = _secsd_script(177, 0x00)
    script.append((r"^2\r$", "2\r\n  EXT_CSD[B1] = 0x00" + PROMPT))
    with pytest.raises(BenchError, match="reads back 0x00"):
        sw.em_secsd(FakeConsole(script), 177, 0x02)


def test_em_secsd_writer_error_raises():
    script = _secsd_script(177, 0x00)
    script.append((r"^2\r$", "2\r\n EM_SECSD ERR!" + PROMPT))
    with pytest.raises(BenchError, match="EM_SECSD ERR!"):
        sw.em_secsd(FakeConsole(script), 177, 0x02)


# --- EM_DCID ---------------------------------------------------------------

CID_OUT = (
    "EM_DCID\r\n\r\n[CID Field Data]\r\n"
    "[127:120]  MID  0x15\r\n"
    "[113:112]  CBX  0x01\r\n"
    "[111:104]  OID  0x00\r\n"
    "[103: 56]  PNM  0x414C50544553\r\n"      # "ALPTES"
    "[ 55: 48]  PRV  0x21\r\n"
    "[ 47: 16]  PSN  0x0A1B2C3D\r\n"
    "[ 15:  8]  MDT  0x93\r\n"
    "[  7:  1]  CRC  0x2A\r\n"
    "\r\n" + PROMPT
)


def test_em_dcid_parses_fields_and_rebuilds_raw():
    con = FakeConsole([(r"^EM_DCID\r$", CID_OUT)])
    cid = sw.em_dcid(con)
    assert cid == {
        "emmc_cid_raw": "150100414c50544553210a1b2c3d9355",
        "emmc_cid_mid": "0x15",
        "emmc_cid_pnm": "ALPTES",
        "emmc_cid_prv": "0x21",
        "emmc_cid_psn": "0x0a1b2c3d",
        "emmc_cid_mdt": "0x93",
    }
    assert len(cid["emmc_cid_raw"]) == 32


def test_em_dcid_init_error_raises():
    con = FakeConsole([(r"^EM_DCID\r$", "EM_DCID\r\neMMC Init ERROR!" + PROMPT)])
    with pytest.raises(BenchError, match="eMMC Init ERROR!"):
        sw.em_dcid(con)


def test_parse_cid_missing_field_raises():
    with pytest.raises(BenchError, match="PSN"):
        sw.parse_cid(CID_OUT.replace("PSN", "XXX"))


# --- the whole bootstrap sequence on one console --------------------------

def test_bootstrap_sequence_on_one_console(tmp_path):
    """Writer load, two EM_W, both EXT_CSD writes and EM_DCID back to back."""
    mot = tmp_path / "writer.mot"
    mot.write_bytes(b"S0030000FC\r\n")
    bl2, fip = b"\x11" * 100, b"\x22" * 5000
    script = [(None, "SCI Download mode\r\n"), (r"(?s).", "Flash writer for RZ/V2N" + PROMPT)]
    done = "SAVE -FLASH.......\r\nEM_W Complete!" + PROMPT
    script += _em_w_script(bl2, 0x8101E00, sw.BL2_MMC_SECTOR, done)
    script += _em_w_script(fip, 0x44000000, sw.FIP_SECTOR, done)
    for index, value in sw.EXT_CSD_WRITES:
        script += _secsd_script(index, 0x00)
        script.append((rf"^{value:X}\r$", f"{value:X}\r\n  EXT_CSD[{index:02X}] = 0x{value:02X}" + PROMPT))
    script.append((r"^EM_DCID\r$", CID_OUT))
    con = FakeConsole(script)

    sw.load_writer(con, mot, timeout=1)
    sw.em_w(con, sw.BOOT1_AREA, sw.BL2_MMC_SECTOR, 0x8101E00, bl2, timeout=1)
    sw.em_w(con, sw.BOOT1_AREA, sw.FIP_SECTOR, 0x44000000, fip, timeout=1)
    for index, value in sw.EXT_CSD_WRITES:
        sw.em_secsd(con, index, value)
    assert sw.em_dcid(con)["emmc_cid_mid"] == "0x15"
    assert not con.script
