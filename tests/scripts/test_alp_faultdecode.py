"""Tests for `alp faultdecode` -- the ARMv8-M (M33/M55) fault decoder.

The decode core is a pure function (``faultdecode.decode``), so the bulk of the
logic is unit-tested directly with register integers -- no ELF, no board, no
shelling out.  A handful of CliRunner tests cover the command wiring, the
``--json`` shape, the dump-greping, the no-fault path and bad input.
"""

from __future__ import annotations

import json

from click.testing import CliRunner

from alp_cli import faultdecode as fd
from alp_cli.main import cli

# A canonical precise BusFault: BFSR.PRECISERR (bit 9) + BFSR.BFARVALID (bit 15).
CFSR_PRECISE_BUS = (1 << 9) | (1 << 15)  # 0x00008200
BFAR = 0xDEADBEEF


def _run(args=None, **kw):
    return CliRunner().invoke(cli, ["faultdecode", *(args or [])], **kw)


# -------- pure decode core ----------------------------------------------------


def test_decode_precise_bus_fault_flags_and_address():
    report = fd.decode(cfsr=CFSR_PRECISE_BUS, bfar=BFAR)
    names = {f.name for f in report.flags}
    assert {"PRECISERR", "BFARVALID"} <= names
    assert report.fault_detected
    assert report.bfar_valid is True
    assert report.bfar == BFAR
    # Each set flag carries its register + a plain-English meaning.
    preciserr = next(f for f in report.flags if f.name == "PRECISERR")
    assert preciserr.reg == "BFSR"
    assert preciserr.meaning  # non-empty


def test_decode_precise_bus_fault_root_cause_mentions_address():
    report = fd.decode(cfsr=CFSR_PRECISE_BUS, bfar=BFAR)
    rc = report.root_cause.lower()
    assert "bus fault" in rc
    assert f"0x{BFAR:08x}" in report.root_cause
    # The classic hint about peripherals / bad pointers is surfaced.
    assert "peripheral" in rc or "pointer" in rc


def test_decode_no_bits_set_reports_no_fault():
    report = fd.decode(cfsr=0, hfsr=0)
    assert report.fault_detected is False
    assert report.flags == []
    assert "no fault" in report.root_cause.lower()


def test_decode_mpu_data_violation_uses_mmfar():
    # MMFSR.DACCVIOL (bit1) + MMFSR.MMARVALID (bit7).
    cfsr = (1 << 1) | (1 << 7)
    report = fd.decode(cfsr=cfsr, mmfar=0x20001000)
    assert report.mmfar_valid is True
    assert "0x20001000" in report.root_cause
    assert "mpu" in report.root_cause.lower()


def test_decode_stack_overflow_wins_over_forced():
    # UFSR.STKOF (bit20) escalated to a forced HardFault (HFSR.FORCED, bit30).
    report = fd.decode(cfsr=(1 << 20), hfsr=(1 << 30))
    assert report.has("STKOF")
    assert report.has("FORCED")
    assert "stack overflow" in report.root_cause.lower()


def test_decode_divbyzero():
    report = fd.decode(cfsr=(1 << 25))  # UFSR.DIVBYZERO
    assert report.has("DIVBYZERO")
    assert "divide by zero" in report.root_cause.lower()


def test_decode_invstate_thumb_bit_hint():
    report = fd.decode(cfsr=(1 << 17))  # UFSR.INVSTATE
    assert "thumb" in report.root_cause.lower()


def test_decode_bfar_without_valid_bit_is_marked_stale():
    # PRECISERR but no BFARVALID -> address reported but not authoritative.
    report = fd.decode(cfsr=(1 << 9), bfar=0x40000000)
    assert report.bfar == 0x40000000
    assert report.bfar_valid is False


def test_decode_hfsr_only_forced():
    report = fd.decode(hfsr=(1 << 30))
    assert report.has("FORCED")
    assert "forced hardfault" in report.root_cause.lower()


# -------- dump parsing --------------------------------------------------------


def test_parse_dump_extracts_named_registers():
    dump = """
    *** HARD FAULT ***
    CFSR: 0x00008200
    HFSR = 0x40000000
    BFAR  0xDEADBEEF
    """
    regs = fd.parse_dump(dump)
    assert regs["cfsr"] == 0x00008200
    assert regs["hfsr"] == 0x40000000
    assert regs["bfar"] == 0xDEADBEEF


def test_parse_dump_composes_cfsr_from_subregisters():
    dump = "MMFSR: 0x02  BFSR: 0x82  UFSR: 0x0000"
    regs = fd.parse_dump(dump)
    # BFSR 0x82 -> bits 8..15, MMFSR 0x02 -> bits 0..7.
    assert regs["cfsr"] == (0x82 << 8) | 0x02
    assert "mmfsr" not in regs and "bfsr" not in regs


def test_parse_dump_mmfar_address_phrasing():
    regs = fd.parse_dump("MMFAR Address: 0x20001000")
    assert regs["mmfar"] == 0x20001000


# -------- json shape ----------------------------------------------------------


def test_report_to_json_shape():
    report = fd.decode(cfsr=CFSR_PRECISE_BUS, bfar=BFAR)
    payload = fd.report_to_json(report, None)
    assert payload["fault_detected"] is True
    assert isinstance(payload["flags"], list)
    assert {"PRECISERR", "BFARVALID"} <= {f["name"] for f in payload["flags"]}
    assert payload["addresses"]["bfar"] == f"0x{BFAR:08x}"
    assert payload["addresses"]["bfar_valid"] is True
    assert payload["root_cause"]
    assert payload["symbols"] is None


# -------- end-to-end command behaviour ---------------------------------------


def test_cmd_precise_bus_fault_human_output():
    result = _run(["--cfsr", "0x00008200", "--bfar", "0xdeadbeef", "--no-color"])
    assert result.exit_code == 0, result.output
    assert "PRECISERR" in result.output
    assert "BFARVALID" in result.output
    assert "0xdeadbeef" in result.output.lower()
    assert "cause" in result.output.lower()


def test_cmd_json_is_machine_readable():
    result = _run(["--cfsr", "0x00008200", "--bfar", "0xdeadbeef", "--json"])
    assert result.exit_code == 0, result.output
    payload = json.loads(result.output)
    assert payload["fault_detected"] is True
    names = {f["name"] for f in payload["flags"]}
    assert {"PRECISERR", "BFARVALID"} <= names
    assert payload["addresses"]["bfar"].lower() == "0xdeadbeef"


def test_cmd_no_fault_flags_path():
    result = _run(["--cfsr", "0x0", "--no-color"])
    assert result.exit_code == 0, result.output
    assert "no fault flags" in result.output.lower()


def test_cmd_bare_hex_accepted():
    # Registers are usually pasted bare; the param type accepts that too.
    result = _run(["--cfsr", "8200", "--bfar", "deadbeef", "--json"])
    assert result.exit_code == 0, result.output
    payload = json.loads(result.output)
    assert {"PRECISERR", "BFARVALID"} <= {f["name"] for f in payload["flags"]}


def test_cmd_reads_dump_from_file(tmp_path):
    dump = tmp_path / "fault.txt"
    dump.write_text("CFSR: 0x00008200\nBFAR: 0xdeadbeef\n", encoding="utf-8")
    result = _run(["--file", str(dump), "--json"])
    assert result.exit_code == 0, result.output
    payload = json.loads(result.output)
    assert {"PRECISERR", "BFARVALID"} <= {f["name"] for f in payload["flags"]}
    assert payload["addresses"]["bfar"].lower() == "0xdeadbeef"


def test_cmd_reads_dump_from_stdin():
    result = _run(["--file", "-", "--json"], input="UFSR: 0x0200\n")  # DIVBYZERO (bit25 -> 0x02 in UFSR hi)
    assert result.exit_code == 0, result.output
    payload = json.loads(result.output)
    assert payload["fault_detected"] is True


def test_cmd_no_registers_is_bad_input():
    # No flags and no dump -> nonzero (this is the only failure mode).
    result = _run([], input="")
    assert result.exit_code != 0
    assert "no fault registers" in result.output.lower()


def test_cmd_bad_value_is_rejected():
    result = _run(["--cfsr", "not-a-number"])
    assert result.exit_code != 0
    assert "valid integer" in result.output.lower()


def test_cmd_pc_without_elf_notes_skip():
    result = _run(["--cfsr", "0x00008200", "--pc", "0x08001234", "--no-color"])
    assert result.exit_code == 0, result.output
    # Symbolication is skipped gracefully with a hint, not a crash.
    assert "--elf" in result.output


def test_cmd_symbolication_via_stubbed_tool(monkeypatch, tmp_path):
    elf = tmp_path / "app.elf"
    elf.write_text("not a real elf", encoding="utf-8")

    monkeypatch.setattr(fd, "resolve_symbol",
                        lambda addr, e: fd.Symbol(addr=addr, func="hard_fault_handler",
                                                  location="src/main.c:42"))
    result = _run(["--cfsr", "0x00008200", "--pc", "0x08001234",
                   "--elf", str(elf), "--json"])
    assert result.exit_code == 0, result.output
    payload = json.loads(result.output)
    assert payload["symbols"]["pc"]["func"] == "hard_fault_handler"
    assert payload["symbols"]["pc"]["location"] == "src/main.c:42"


def test_resolve_symbol_no_tool_returns_none(monkeypatch, tmp_path):
    elf = tmp_path / "app.elf"
    elf.write_text("x", encoding="utf-8")
    monkeypatch.setattr(fd.shutil, "which", lambda _: None)
    assert fd.resolve_symbol(0x08001234, elf) is None


def test_registered_in_help():
    result = CliRunner().invoke(cli, ["--help"])
    assert result.exit_code == 0
    assert "faultdecode" in result.output
