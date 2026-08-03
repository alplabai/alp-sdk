"""Unit tests for scripts/gen_soc_peripheral_instances.py.

Covers the DTSI extraction regexes against a small synthetic fixture (no
real Zephyr checkout needed), splice determinism/idempotency, and the
--check gate against the real committed metadata/socs/renesas/rzv2n/n44.json
-- the last group skips cleanly when no Zephyr checkout resolves, the same
convention the generator itself uses (see check_emit_kconfig_contract.py).
"""
from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

import gen_soc_peripheral_instances as gspi  # noqa: E402  (scripts/ on sys.path via conftest)

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "gen_soc_peripheral_instances.py"

# A trimmed, syntactically-real slice of r9a09g056.dtsi: one i2c node with
# the multi-line `interrupts` continuation the real file uses, one gpt node
# with a `DT_SIZE_K`-free literal reg size, and a nested `pwm { ... };`
# child (three-tab close) to prove the two-tab node-close anchor does not
# stop early on it.
_FIXTURE_DTSI = (
    "\tsoc {\n"
    "\t\ti2c0: i2c@44400400 {\n"
    "\t\t\tcompatible = \"renesas,rz-riic\";\n"
    "\t\t\tchannel = <0>;\n"
    "\t\t\treg = <0x44400400 DT_SIZE_K(1)>;\n"
    "\t\t\tinterrupts = <174 1>, <175 1>,\n"
    "\t\t\t\t     <176 1>, <177 1>;\n"
    "\t\t\tinterrupt-names = \"tei\", \"naki\", \"spi\", \"sti\";\n"
    "\t\t\tstatus = \"disabled\";\n"
    "\t\t};\n"
    "\n"
    "\t\tgpt0: gpt@43010000 {\n"
    "\t\t\tcompatible = \"renesas,rz-gpt\";\n"
    "\t\t\treg = <0x43010000 0x100>;\n"
    "\t\t\tchannel = <0>;\n"
    "\t\t\tinterrupts = <406 1>, <407 1>, <408 1>;\n"
    "\t\t\tinterrupt-names = \"ccmpa\", \"ccmpb\", \"ovf\";\n"
    "\t\t\tstatus = \"disabled\";\n"
    "\n"
    "\t\t\tpwm {\n"
    "\t\t\t\tcompatible = \"renesas,rz-gpt-pwm\";\n"
    "\t\t\t\tstatus = \"disabled\";\n"
    "\t\t\t};\n"
    "\t\t};\n"
    "\n"
    "\t\tadc0: adc0@41c00000 {\n"
    "\t\t\tcompatible = \"renesas,rz-adc-e\";\n"
    "\t\t\treg = <0x41c00000 0x400>;\n"
    "\t\t\tunit = <0>;\n"
    "\t\t\tinterrupts = <403 3>;\n"
    "\t\t\tinterrupt-names = \"scanend\";\n"
    "\t\t\tstatus = \"disabled\";\n"
    "\t\t};\n"
    "\t};\n"
)

_CLASSES = {"renesas,rz-riic": "i2c", "renesas,rz-gpt": "timer_32bit_gpt"}
_GRANULARITY_MISMATCH = {"renesas,rz-adc-e": "adc_12bit (test fixture)"}


def test_extract_instances_grounds_reg_and_interrupts():
    by_key, skips = gspi._extract_instances(_FIXTURE_DTSI, _CLASSES, _GRANULARITY_MISMATCH)

    assert list(by_key["i2c"]) == [
        {
            "index": 0, "label": "i2c0", "base": "0x44400400", "size": "0x400",
            "interrupts": [
                {"irq": 174, "priority": 1, "name": "tei"},
                {"irq": 175, "priority": 1, "name": "naki"},
                {"irq": 176, "priority": 1, "name": "spi"},
                {"irq": 177, "priority": 1, "name": "sti"},
            ],
            "compatible": "renesas,rz-riic",
        }
    ]
    assert by_key["timer_32bit_gpt"] == [
        {
            "index": 0, "label": "gpt0", "base": "0x43010000", "size": "0x100",
            "interrupts": [
                {"irq": 406, "priority": 1, "name": "ccmpa"},
                {"irq": 407, "priority": 1, "name": "ccmpb"},
                {"irq": 408, "priority": 1, "name": "ovf"},
            ],
            "compatible": "renesas,rz-gpt",
        }
    ]


def test_extract_instances_does_not_stop_early_on_nested_child_close():
    """The gpt0 node's `pwm { ... };` child closes at three tabs; the parser
    must still capture gpt0's own fields (interrupt-names past the child)."""
    by_key, _skips = gspi._extract_instances(_FIXTURE_DTSI, _CLASSES, _GRANULARITY_MISMATCH)
    assert by_key["timer_32bit_gpt"][0]["interrupts"][2]["name"] == "ovf"


def test_extract_instances_reports_granularity_mismatch_skip():
    _by_key, skips = gspi._extract_instances(_FIXTURE_DTSI, _CLASSES, _GRANULARITY_MISMATCH)
    assert any("renesas,rz-adc-e" in s and "granularity mismatch" in s for s in skips)


def test_extract_instances_reports_not_found_skip():
    """A compatible absent from the fixture is reported as not-found, not
    silently omitted."""
    _by_key, skips = gspi._extract_instances(
        _FIXTURE_DTSI, _CLASSES, {"renesas,rz-nonexistent": "made up (test)"})
    assert any("renesas,rz-nonexistent" in s and "not found" in s for s in skips)


def test_render_block_is_deterministic():
    by_key, _skips = gspi._extract_instances(_FIXTURE_DTSI, _CLASSES, {})
    assert gspi._render_block(by_key) == gspi._render_block(by_key)


def test_splice_inserts_after_peripherals_block_and_is_idempotent():
    doc_text = (
        '{\n'
        '  "peripherals": {\n'
        '    "i2c": 1\n'
        '  },\n'
        '  "capabilities": {\n'
        '    "neon": true\n'
        '  }\n'
        '}\n'
    )
    by_key, _skips = gspi._extract_instances(_FIXTURE_DTSI, {"renesas,rz-riic": "i2c"}, {})

    once = gspi._splice(doc_text, by_key)
    parsed = json.loads(once)
    assert parsed["peripheral_instances"]["i2c"][0]["label"] == "i2c0"
    # capabilities (and everything else outside the spliced region) must be
    # untouched byte-for-byte.
    assert '"capabilities": {\n    "neon": true\n  }' in once

    twice = gspi._splice(once, by_key)
    assert twice == once


def test_size_macro_and_literal_both_resolve():
    assert gspi._parse_size("DT_SIZE_K(1)") == 1024
    assert gspi._parse_size("0x100") == 256
    assert gspi._parse_size("400") == 400
    assert gspi._parse_size("DT_SIZE_K(garbage)") is None


def test_hex_formats_lowercase_0x_prefixed():
    assert gspi._hex(0x41C01000) == "0x41c01000"
    assert gspi._hex(0) == "0x0"


def test_parse_node_emits_hex_strings_not_decimal_ints():
    """base/size must be hex strings (reviewable against the DTSI at a
    glance); index/irq/priority stay decimal ints, matching how the DTSI
    and Zephyr itself write them (`channel = <0>`, `interrupts = <406 1>`).
    """
    by_key, _skips = gspi._extract_instances(_FIXTURE_DTSI, _CLASSES, {})
    inst = by_key["i2c"][0]
    assert isinstance(inst["base"], str) and inst["base"] == "0x44400400"
    assert isinstance(inst["size"], str) and inst["size"] == "0x400"
    assert isinstance(inst["index"], int)
    assert isinstance(inst["interrupts"][0]["irq"], int)
    assert isinstance(inst["interrupts"][0]["priority"], int)


# ---- Integration against the real committed metadata (needs Zephyr) ------

def _zephyr_available() -> bool:
    zdir = gspi._resolve_zephyr_dir()
    return (zdir / gspi._TARGETS[0]["dtsi_relpath"]).is_file()


@pytest.mark.skipif(not _zephyr_available(),
                     reason="needs a resolvable Zephyr checkout (ZEPHYR_BASE "
                            "or <west-topdir>/zephyr) -- see the module docstring")
def test_check_mode_passes_on_committed_n44_json():
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--check"],
        capture_output=True, text=True, cwd=REPO,
    )
    assert proc.returncode == 0, proc.stdout + proc.stderr


@pytest.mark.skipif(not _zephyr_available(),
                     reason="needs a resolvable Zephyr checkout (ZEPHYR_BASE "
                            "or <west-topdir>/zephyr) -- see the module docstring")
def test_check_mode_fails_when_an_instance_is_corrupted(tmp_path, monkeypatch):
    soc_json = gspi._TARGETS[0]["soc_json"]
    corrupted = tmp_path / soc_json.name
    text = soc_json.read_text(encoding="utf-8")
    needle = '"base": "0x41c01000"'  # i2c8 / RIIC8 / BRD_I2C
    assert needle in text, "fixture assumption stale -- re-check i2c8's base"
    corrupted.write_text(text.replace(needle, '"base": "0x41c01001"', 1), encoding="utf-8")

    fake_target = dict(gspi._TARGETS[0])
    fake_target["soc_json"] = corrupted
    monkeypatch.setattr(gspi, "_TARGETS", [fake_target])
    monkeypatch.setattr(sys, "argv", ["gen_soc_peripheral_instances.py", "--check"])
    assert gspi.main() == 1


def test_committed_n44_json_rejects_decimal_or_uppercase_base(monkeypatch):
    """Schema mutation proof (PR review, #1154): a decimal int or an
    uppercase-hex string in `peripheral_instances[*][*].base` must fail
    `metadata/schemas/soc-spec-v1.schema.json` validation -- this is the
    guard that stops the next transposed-digit address from being silently
    accepted, independent of whether `--check` itself ran."""
    jsonschema = pytest.importorskip("jsonschema")
    schema = json.loads(
        (REPO / "metadata" / "schemas" / "soc-spec-v1.schema.json").read_text(encoding="utf-8"))
    doc = json.loads(
        (REPO / "metadata" / "socs" / "renesas" / "rzv2n" / "n44.json").read_text(encoding="utf-8"))
    validator = jsonschema.Draft202012Validator(schema)
    assert validator.is_valid(doc)

    decimal = json.loads(json.dumps(doc))
    decimal["peripheral_instances"]["i2c"][8]["base"] = 1103106048
    assert not validator.is_valid(decimal)

    uppercase = json.loads(json.dumps(doc))
    uppercase["peripheral_instances"]["i2c"][8]["base"] = "0x41C01000"
    assert not validator.is_valid(uppercase)
