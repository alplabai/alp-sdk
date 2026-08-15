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


def test_alp_prefixed_bus_spelling_is_matched(tmp_path):
    """`ALP_E1M_I2C0` is the dominant, portable-API spelling for the
    `e1m_i2c0` bus key (66 sites in the real tree, 0 for the bare
    `E1M_I2C0` -- and #1270's own issue text uses this spelling). A
    bare `\\bE1M_I2C0\\b` regex can never match it -- the `_` right
    before `E1M` is itself a word character, so there is no boundary
    there. Must be caught the same way the bare literal is."""
    _write_preset(tmp_path, "E1M-TEST")
    _write_doc(
        tmp_path, "docs/x.md",
        "eeprom_24c128 lives on ALP_E1M_I2C0 per the SoM preset.\n",
    )
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode == 0, out  # eeprom_24c128's TRUE bus IS e1m_i2c0

    _write_doc(
        tmp_path, "docs/y.md",
        "tmp112 lives on ALP_E1M_I2C0 per the SoM preset.\n",
    )
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode == 1, out  # tmp112's TRUE bus is brd_i2c, not e1m_i2c0
    assert "tmp112" in out
    assert "e1m_i2c0" in out


def test_lpi2c_without_trailing_zero_is_an_alias(tmp_path):
    """`docs/soms/aen.md`'s own Bus column spells the `brd_i2c` bus
    'LPI2C' (no trailing '0') for three of its four rows -- an alias
    table carrying only 'LPI2C0' leaves those rows invisible to the
    gate. Prove the alias actually participates in mismatch detection
    (not merely fails to error)."""
    _write_preset(tmp_path, "E1M-TEST")
    _write_doc(
        tmp_path, "docs/x.md",
        "The 24C128 EEPROM sits on the Alif LPI2C bus.\n",
    )
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode == 1, out  # eeprom_24c128's TRUE bus is e1m_i2c0
    assert "eeprom_24c128" in out
    assert "brd_i2c" in out


def test_generic_name_part_does_not_match_inside_an_unrelated_word(tmp_path):
    """`optiga_trust_m` contributes the alias `TRUST` (len 5, not in
    `_GENERIC_PARTS`); a whole-line-squash substring search lets it
    false-positive inside `TRUSTED`/`TRUSTZONE` once punctuation is
    stripped -- a false positive on a BLOCKING gate, proven on real
    prose ('the TrustZone-M secure partition' is a real Alif
    TrustZone-M term). Token-exact matching must not flag either
    sentence, even though each names a real ground-truth bus."""
    # optiga_trust_m's TRUE bus is brd_i2c (matching the real
    # E1M-AEN801.yaml ground truth). `e1m_i2c0` also needs a device of
    # its OWN (eeprom_24c128) so that bus's pattern (`I2C2`) exists at
    # all -- `_bus_pattern` is only built for buses that appear as some
    # chip's true bus, so a single-chip-single-bus preset would make
    # `I2C2` unrecognised for a reason having nothing to do with the
    # alias bug this test targets. Both false-positive lines below name
    # `I2C2` (-> e1m_i2c0) -- the WRONG bus for optiga_trust_m -- which
    # is exactly the condition needed to prove whether the chip alias
    # match fires at all.
    d = tmp_path / "metadata" / "e1m_modules"
    d.mkdir(parents=True, exist_ok=True)
    (d / "E1M-TEST.yaml").write_text(
        "sku: E1M-TEST\n"
        "on_module:\n"
        "  i2c_devices:\n"
        "    brd_i2c:\n"
        "      devices:\n"
        "        - { chip: optiga_trust_m, role: secure_element, address_7bit: \"0x30\" }\n"
        "    e1m_i2c0:\n"
        "      devices:\n"
        "        - { chip: eeprom_24c128, role: eeprom, address_7bit: \"0x50\" }\n",
        encoding="utf-8",
    )
    _write_doc(
        tmp_path, "docs/x.md",
        "The trusted-boot flow runs before I2C2 comes up.\n",
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr

    _write_doc(
        tmp_path, "docs/y.md",
        "The TrustZone-M secure partition owns I2C2 on this target.\n",
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr

    # Sanity: the SAME alias must still catch a genuine, correctly
    # word-bounded mention naming the wrong bus -- the fix must not
    # have traded the false positive for a false negative.
    _write_doc(
        tmp_path, "docs/z.md",
        "OPTIGA Trust M is reached over I2C2 on this board.\n",
    )
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode == 1, out
    assert "optiga_trust_m" in out


def test_chip_name_alone_with_no_bus_is_not_flagged(tmp_path):
    """A chip name mentioned with NO bus name on the same line is inert
    -- this gate makes no claim at all without a co-located bus, on
    round 3's now-removed address check or otherwise. (Round 4, #1270:
    round 3's address check -- gating a bare `0xNN` literal on a
    co-located single bus and single chip -- was dropped outright: no
    line-level signal cheap enough for this gate reliably distinguishes
    a 7-bit I2C address from a register offset/bitmask/length that
    happens to share the same two-hex-digit shape. See the module
    docstring's Scope section.)"""
    _write_preset(tmp_path, "E1M-TEST")
    _write_doc(
        tmp_path, "docs/x.md",
        "/* tmp112 I2C_STATE register (0x82) reads 4 bytes */\n",
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_register_value_on_a_correct_chip_bus_line_is_not_a_false_address_claim(
    tmp_path,
):
    """Round 4 (#1270) review: round 3's address check flagged ANY
    `0xNN` literal on a line naming exactly one ground-truth chip and
    exactly one (CORRECT) bus, with no evidence the literal was ever an
    I2C address claim rather than a register offset/bitmask/length that
    happens to share the same two-hex-digit shape. Proven against the
    pre-round-4 script (git archive `8f5c2728`, this exact fixture):
    it reported 'names tmp112 at address 0x49 ... but ... puts tmp112
    at 0x48' even though the line's `0x49` is a register value, not an
    address claim, and the doc's BUS claim (BRD_I2C) was already
    correct. The address check is dropped outright (no cheap reliable
    signal distinguishes the two); this line must never be flagged."""
    _write_preset(tmp_path, "E1M-TEST")
    _write_doc(
        tmp_path, "docs/x.md",
        "TMP112 config register 0x49 resets to default on BRD_I2C.\n",
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_generic_dictionary_word_alias_is_dropped(tmp_path):
    """`optiga_trust_m`'s underscore-part `trust` is an ordinary English
    dictionary word, not a specific product term -- unlike
    `trusted-boot`/`TrustZone-M` (caught by token-exact matching, tested
    above), a BARE `trust` token in unrelated prose is syntactically a
    real, standalone token and still false-positived even with the
    substring bug fixed (review round 2: 'You can trust the I2C2 scan
    output on a freshly booted module.' named the wrong bus for
    `optiga_trust_m`). `trust` must be excluded from `_GENERIC_PARTS`
    the same way `eeprom` is; the chip's SPECIFIC alias (`OPTIGA`) must
    still catch a real, correctly-bounded mention."""
    d = tmp_path / "metadata" / "e1m_modules"
    d.mkdir(parents=True, exist_ok=True)
    (d / "E1M-TEST.yaml").write_text(
        "sku: E1M-TEST\n"
        "on_module:\n"
        "  i2c_devices:\n"
        "    brd_i2c:\n"
        "      devices:\n"
        "        - { chip: optiga_trust_m, role: secure_element, address_7bit: \"0x30\" }\n"
        "    e1m_i2c0:\n"
        "      devices:\n"
        "        - { chip: eeprom_24c128, role: eeprom, address_7bit: \"0x50\" }\n",
        encoding="utf-8",
    )
    _write_doc(
        tmp_path, "docs/x.md",
        "You can trust the I2C2 scan output on a freshly booted module.\n",
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr  # bare 'trust' must not match

    # Sanity: the chip's real, specific alias must still catch a genuine
    # mismatch -- the fix must not have silenced the chip entirely.
    _write_doc(
        tmp_path, "docs/y.md",
        "OPTIGA is reached over I2C2 on this board.\n",
    )
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode == 1, out
    assert "optiga_trust_m" in out


def test_archival_superpowers_plans_doc_is_out_of_scope(tmp_path):
    """`docs/superpowers/plans/**` records dated bench-session planning
    notes -- a PAST state, deliberately (same carve-out
    `check_cross_platform.py`'s `DEFAULT_EXCLUDES` already makes). A
    later, correct metadata change must not force an edit to a document
    recording what was true when it was written (review round 2:
    flipping tmp112's bus reddened 3 real `docs/superpowers/plans/**`
    sites that were accurate history). `docs/superpowers/specs/` carries
    the same pre-cleanup "before" carve-out and is excluded too."""
    _write_preset(tmp_path, "E1M-TEST")
    _write_doc(
        tmp_path, "docs/superpowers/plans/2026-01-01-old-session.md",
        "TMP112 sits on I2C2 (bench notes from that day).\n",
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr

    _write_doc(
        tmp_path, "docs/superpowers/specs/pre-cleanup-design.md",
        "TMP112 sits on I2C2 in the old design.\n",
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr

    # Sanity: a LIVE doc naming the same wrong bus must still fire --
    # the exclusion is path-scoped, not chip/bus-scoped.
    _write_doc(tmp_path, "docs/live.md", "TMP112 sits on I2C2.\n")
    proc = _run("--root", str(tmp_path))
    out = proc.stdout + proc.stderr
    assert proc.returncode == 1, out
    assert "tmp112" in out


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
