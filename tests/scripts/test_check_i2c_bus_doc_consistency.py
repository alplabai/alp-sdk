# SPDX-License-Identifier: Apache-2.0
"""Unit tests for scripts/check_i2c_bus_doc_consistency.py.

Builds a MINIMAL synthetic tree per test (one SoM preset's
`on_module.i2c_devices` block + one doc file) rather than copying the
real corpus -- the gate's own docstring names the exact failure mode
(#1270's wave-4 doc churn); each test reproduces one facet of it against
a fixture small enough to read at a glance.
"""
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_i2c_bus_doc_consistency.py"


def _run(*args, **kw):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, **kw,
    )


def _write_preset(tmp_path: Path, sku: str) -> None:
    """One SoM preset with the same two-bus shape E1M-AEN801.yaml carries
    post-#1270: `tmp112` truthfully on `brd_i2c`, `eeprom_24c128`
    truthfully on `e1m_i2c0`."""
    d = tmp_path / "metadata" / "e1m_modules"
    d.mkdir(parents=True, exist_ok=True)
    (d / f"{sku}.yaml").write_text(
        "sku: " + sku + "\n"
        "on_module:\n"
        "  i2c_devices:\n"
        "    brd_i2c:\n"
        "      devices:\n"
        "        - { chip: tmp112, role: temp_sensor, address_7bit: \"0x48\" }\n"
        "    e1m_i2c0:\n"
        "      devices:\n"
        "        - { chip: eeprom_24c128, role: eeprom, address_7bit: \"0x50\" }\n",
        encoding="utf-8",
    )


def _write_doc(tmp_path: Path, rel: str, text: str) -> Path:
    p = tmp_path / rel
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(text, encoding="utf-8")
    return p


def test_empty_tree_passes(tmp_path):
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_clean_doc_agreeing_with_metadata_passes(tmp_path):
    _write_preset(tmp_path, "E1M-TEST")
    _write_doc(tmp_path, "docs/x.md", "The TMP112 thermometer sits on BRD_I2C.\n")
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0, out
    assert "OK" in out


def test_wrong_bus_alias_fails(tmp_path):
    """The exact wave-4 shape #1270 describes: a chip's true bus is
    `brd_i2c` (BRD_I2C/LPI2C0), but the doc names the OTHER bus's
    human alias (I2C2, resolving to `e1m_i2c0`) on the same line."""
    _write_preset(tmp_path, "E1M-TEST")
    _write_doc(
        tmp_path, "docs/x.md",
        "The on-module TMP112 thermometer sits on I2C2 alongside the EEPROM.\n",
    )
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode == 1, out
    assert "tmp112" in out
    assert "e1m_i2c0" in out
    assert "brd_i2c" in out


def test_wrong_bus_key_literal_fails(tmp_path):
    """Same failure, using the bus KEY's own literal spelling
    (`e1m_i2c0`) rather than its human alias (`I2C2`)."""
    _write_preset(tmp_path, "E1M-TEST")
    _write_doc(
        tmp_path, "docs/x.md",
        "eeprom_24c128 lives on brd_i2c per the SoM preset.\n",
    )
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode == 1, out
    assert "eeprom_24c128" in out


def test_line_naming_both_buses_is_skipped_not_flagged(tmp_path):
    """A contrastive line naming BOTH buses ('I2C2, NOT LPI2C0') is
    deliberately ambiguous and must never be flagged -- this is the
    exact shape of docs/soms/aen.md's real EEPROM row."""
    _write_preset(tmp_path, "E1M-TEST")
    _write_doc(
        tmp_path, "docs/x.md",
        "TMP112 sits on I2C2, NOT LPI2C0.\n",
    )
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0, out


def test_ambiguous_chip_across_presets_is_never_ground_truth(tmp_path):
    """A chip declared on DIFFERENT buses by two presets is ambiguous
    ground truth -- excluded from checking entirely, never guessed at,
    even when a doc line would otherwise look like a clean mismatch."""
    _write_preset(tmp_path, "E1M-A")
    d = tmp_path / "metadata" / "e1m_modules"
    (d / "E1M-B.yaml").write_text(
        "sku: E1M-B\n"
        "on_module:\n"
        "  i2c_devices:\n"
        "    e1m_i2c0:\n"
        "      devices:\n"
        "        - { chip: tmp112, role: temp_sensor, address_7bit: \"0x48\" }\n",
        encoding="utf-8",
    )
    _write_doc(tmp_path, "docs/x.md", "TMP112 sits on I2C2.\n")
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0, out


def test_unrelated_chip_name_not_flagged(tmp_path):
    """A line naming a bus but no ground-truth chip name is inert."""
    _write_preset(tmp_path, "E1M-TEST")
    _write_doc(tmp_path, "docs/x.md", "The board boots over I2C2.\n")
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0, out


def test_example_c_source_is_scanned_too(tmp_path):
    """The gate's own scope explicitly includes examples/**/*.c (main.c
    teaching comments), not just docs/**/*.md."""
    _write_preset(tmp_path, "E1M-TEST")
    _write_doc(
        tmp_path, "examples/aen/aen-secure-element-sign/src/main.c",
        "/* eeprom_24c128 is reached over BRD_I2C on this board. */\n",
    )
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode == 1, out
    assert "eeprom_24c128" in out
