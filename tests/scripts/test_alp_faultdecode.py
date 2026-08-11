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


# -------- issue #1358: HFSR.FORCED is an escalation, not a root cause ---------
#
# Register values below are transcribed verbatim from issue #1358's own
# reproduction (`faultdecode --cfsr 0x2000 --hfsr 0x40000000`), a real
# ARMv8-M fault shape: BFSR.LSPERR (bit 13, 0x2000) / MMFSR.MLSPERR (bit 5,
# 0x20) escalated to HardFault via HFSR.FORCED (bit 30, 0x40000000).


def test_forced_no_longer_reported_as_root_cause_when_lsperr_is_set():
    # Pre-fix, this decoded to "Forced HardFault -- ... its own status bits
    # are clear" -- false, since LSPERR (bit 13) IS set. LSPERR must now win.
    report = fd.decode(cfsr=0x2000, hfsr=0x40000000)
    assert report.has("LSPERR")
    assert report.has("FORCED")  # the escalation is still reported as a flag
    rc = report.root_cause.lower()
    assert "lsperr" not in rc  # the flag name isn't echoed, the meaning is
    assert "forced hardfault" not in rc
    assert "status bits are clear" not in rc  # the false clause is gone
    assert "lazily preserving the floating-point context" in rc


def test_forced_no_longer_reported_as_root_cause_when_mlsperr_is_set():
    # Same defect, the MMFSR sibling: --cfsr 0x20 --hfsr 0x40000000.
    report = fd.decode(cfsr=0x20, hfsr=0x40000000)
    assert report.has("MLSPERR")
    assert report.has("FORCED")
    rc = report.root_cause.lower()
    assert "forced hardfault" not in rc
    assert "status bits are clear" not in rc
    assert "memmanage fault while lazily preserving" in rc


def test_lsperr_root_cause_carries_the_bfar_address():
    # The generic fallback the LSPERR/MLSPERR bits used to fall through to
    # discarded BFAR/MMFAR; the dedicated branch must not repeat that.
    report = fd.decode(cfsr=0x2000 | (1 << 15), hfsr=0x40000000, bfar=0xDEADBEEF)
    assert report.bfar_valid is True
    assert "0xdeadbeef" in report.root_cause.lower()


def test_mlsperr_root_cause_carries_the_mmfar_address():
    report = fd.decode(cfsr=0x20 | (1 << 7), hfsr=0x40000000, mmfar=0xCAFEBABE)
    assert report.mmfar_valid is True
    assert "0xcafebabe" in report.root_cause.lower()


def test_forced_still_headlines_when_cfsr_names_no_cause():
    # The escalation-vs-cause guard is keyed on whether CFSR carries a real
    # cause bit, not on removing FORCED outright: with CFSR clear, FORCED's
    # own "status bits are clear" clause is TRUE and must still be reported.
    report = fd.decode(cfsr=0, hfsr=0x40000000)
    assert report.has("FORCED")
    rc = report.root_cause.lower()
    assert "forced hardfault" in rc
    assert "status bits are clear" in rc


def test_vecttbl_outranks_lsperr_and_mlsperr():
    # Two-bit precedence: VECTTBL (HFSR bit1, 0x2) is a more specific finding
    # than a lazy-FP-preservation fault and must keep winning.
    assert "vector-table" in fd.decode(cfsr=0x2000, hfsr=0x2).root_cause.lower()
    assert "vector-table" in fd.decode(cfsr=0x20, hfsr=0x2).root_cause.lower()


def test_debugevt_outranks_lsperr_and_mlsperr():
    # HFSR bit31 (0x80000000).
    assert "debug event" in fd.decode(cfsr=0x2000, hfsr=0x80000000).root_cause.lower()
    assert "debug event" in fd.decode(cfsr=0x20, hfsr=0x80000000).root_cause.lower()


def test_cmd_lsperr_forced_human_output_names_lsperr_not_forced():
    result = _run(["--cfsr", "0x2000", "--hfsr", "0x40000000", "--no-color"])
    assert result.exit_code == 0, result.output
    assert "status bits are clear" not in result.output.lower()
    assert "lazily preserving the floating-point context" in result.output.lower()
    # The escalation itself is still visible under "Set flags:".
    assert "[HFSR] FORCED (bit 30)" in result.output


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


def test_cmd_negative_value_is_rejected():
    # Fault registers are unsigned; int(text, 16) happily parses a leading
    # '-' and would otherwise decode a bogus cause with confidence.
    result = _run(["--cfsr", "-8200"])
    assert result.exit_code != 0
    assert "negative" in result.output.lower()


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
