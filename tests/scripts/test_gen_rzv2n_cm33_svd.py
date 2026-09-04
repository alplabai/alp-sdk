"""Unit tests for scripts/gen_rzv2n_cm33_svd.py (issue #1029 step 2).

Runs entirely against the small, committed fixture under
`tests/scripts/fixtures/rzv2n_svd/` -- no west workspace, no real
hal_renesas checkout needed. The fixture is a trimmed, syntactically-real
excerpt of `gpio_iodefine.h` / `gpio_iobitmask.h` / `R9A09G056N.h` (see the
header comment of each fixture file for exactly what was kept and why),
chosen to exercise a plain register, a register array, a nested cluster
type, and RESERVED padding in one small struct.

The mutation tests are the load-bearing ones: the whole point of
`gen_rzv2n_cm33_svd.py` is that a disagreement between `iodefines/` and
`iobitmasks/` is IMPOSSIBLE to silently miss, so these tests prove the
check can actually fail, not just that the happy path is green.
"""
from __future__ import annotations

import shutil
import subprocess
import sys
from pathlib import Path

import pytest

import gen_rzv2n_cm33_svd as g  # noqa: E402  (scripts/ on sys.path via conftest)

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "gen_rzv2n_cm33_svd.py"
FIXTURE_ROOT = Path(__file__).resolve().parent / "fixtures" / "rzv2n_svd" / "Include" / "R9A09G056N"


def _run(fsp_dir: Path, *extra_args: str) -> subprocess.CompletedProcess:
    return subprocess.run(
        [sys.executable, str(SCRIPT), "--fsp-include-dir", str(fsp_dir), *extra_args],
        capture_output=True,
        text=True,
        timeout=30,
    )


def _copy_fixture(tmp_path: Path) -> Path:
    """A writable copy of the fixture tree, so mutation tests never touch
    the committed fixture files. Mirrors the real layout: the module dir
    AND its sibling `<name>.h` CPU-config header one level up."""
    dest = tmp_path / "R9A09G056N"
    shutil.copytree(FIXTURE_ROOT, dest)
    shutil.copy(FIXTURE_ROOT.parent / "R9A09G056N.h", tmp_path / "R9A09G056N.h")
    return dest


# --- golden: fixture -> expected SVD fragment, numeric offsets/widths ------


def test_generate_against_fixture_produces_expected_offsets_and_widths():
    svd, stats = g.generate(FIXTURE_ROOT)
    assert stats["peripherals"] == 1

    # RESERVED[4] padding pushes the first real member (the PDBF cluster)
    # to offset 0x4, not 0x0.
    assert "<addressOffset>0x4</addressOffset>" in svd
    assert "<name>PDBF[%s]</name>" in svd
    assert "<dim>2</dim>" in svd
    # R_ELC_PDBF_Type is 1 byte (ELC_PDBF) + 3 bytes (RESERVED32[3]) = 4.
    assert "<dimIncrement>0x4</dimIncrement>" in svd
    # The nested register itself starts at offset 0 WITHIN the cluster.
    assert "<name>ELC_PDBF</name>" in svd

    # ELC_PEL[4] register array starts right after the 2*4-byte PDBF
    # cluster (0x4 + 0x8 = 0xC), dim=4, 1 byte per element.
    assert "<name>ELC_PEL[%s]</name>" in svd
    assert "<addressOffset>0xC</addressOffset>" in svd
    assert "<dim>4</dim>" in svd
    assert "<dimIncrement>0x1</dimIncrement>" in svd

    # PFC_ELC_ELSR2 is a plain (non-array) register right after ELC_PEL's
    # 4 bytes (0xC + 0x4 = 0x10).
    assert "<name>PFC_ELC_ELSR2</name>" in svd
    assert "<addressOffset>0x10</addressOffset>" in svd

    # Field positions/widths, cross-checked against the fixture's own
    # iobitmask macros (both sides copied verbatim from the real vendor
    # header pair, see the fixture files' own header comments).
    for name, lsb, width in (("PSB", 0, 3), ("PSP", 3, 2), ("PSM", 5, 2)):
        assert f"<name>{name}</name>" in svd
    assert "<bitOffset>2</bitOffset>" in svd and "<bitWidth>2</bitWidth>" in svd  # PEG
    assert "<bitOffset>4</bitOffset>" in svd and "<bitWidth>4</bitWidth>" in svd  # PES

    assert svd.startswith('<?xml version="1.0" encoding="utf-8"?>')
    assert "<revision>r0p4</revision>" in svd
    assert "<nvicPrioBits>7</nvicPrioBits>" in svd


def test_check_mode_on_fixture_exits_zero_and_emits_nothing(tmp_path):
    proc = _run(FIXTURE_ROOT, "--check")
    assert proc.returncode == 0, proc.stderr
    assert "1 peripherals from 1 iodefine files" in proc.stdout


def test_output_mode_requires_output_flag_unless_check():
    proc = subprocess.run(
        [sys.executable, str(SCRIPT), "--fsp-include-dir", str(FIXTURE_ROOT)],
        capture_output=True,
        text=True,
        timeout=30,
    )
    assert proc.returncode != 0
    assert "--output is required" in proc.stderr


def test_missing_fsp_dir_fails_naming_the_path(tmp_path):
    missing = tmp_path / "does_not_exist"
    proc = _run(missing, "--check")
    assert proc.returncode != 0
    assert str(missing) in proc.stderr


# --- red-then-green mutation tests: prove the cross-check CAN fail ---------


def test_a_disagreeing_pos_is_rejected(tmp_path):
    """Mutate the fixture's iobitmasks copy so PSB's _Pos disagrees with
    the iodefine struct layout -- the generator must reject it and name
    PSB. (Neutering `mask_width`'s pos/width comparison in
    `cross_check_file` makes this test itself fail -- confirmed by hand
    while developing this test, not asserted here since that would require
    editing the generator from the test.)"""
    fsp_dir = _copy_fixture(tmp_path)
    bitmask = fsp_dir / "iobitmasks" / "gpio_iobitmask.h"
    text = bitmask.read_text()
    assert "R_GPIO_ELC_PEL_PSB_Pos          (0UL)" in text
    mutated = text.replace(
        "R_GPIO_ELC_PEL_PSB_Pos          (0UL)", "R_GPIO_ELC_PEL_PSB_Pos          (1UL)"
    )
    assert mutated != text
    bitmask.write_text(mutated)

    proc = _run(fsp_dir, "--check")
    assert proc.returncode != 0
    assert "PSB" in proc.stderr
    assert "ELC_PEL" in proc.stderr


def test_a_field_removed_from_iobitmasks_is_rejected(tmp_path):
    """Delete PSM's _Pos/_Msk macros from the iobitmasks copy -- a field
    iodefine has but iobitmasks doesn't must be rejected by name."""
    fsp_dir = _copy_fixture(tmp_path)
    bitmask = fsp_dir / "iobitmasks" / "gpio_iobitmask.h"
    lines = bitmask.read_text().splitlines(keepends=True)
    kept = [ln for ln in lines if "PSM" not in ln]
    assert len(kept) < len(lines)
    bitmask.write_text("".join(kept))

    proc = _run(fsp_dir, "--check")
    assert proc.returncode != 0
    assert "PSM" in proc.stderr


def test_an_orphaned_iobitmask_macro_is_rejected(tmp_path):
    """The reverse direction: a macro in iobitmasks with no iodefine
    counterpart at all must also be rejected."""
    fsp_dir = _copy_fixture(tmp_path)
    bitmask = fsp_dir / "iobitmasks" / "gpio_iobitmask.h"
    text = bitmask.read_text()
    text += (
        "\n#define R_GPIO_NOSUCHREG_GHOST_Msk    (0x01UL)\n"
        "#define R_GPIO_NOSUCHREG_GHOST_Pos    (0UL)\n"
    )
    bitmask.write_text(text)

    proc = _run(fsp_dir, "--check")
    assert proc.returncode != 0
    assert "NOSUCHREG" in proc.stderr


def test_a_uint64_member_is_rejected(tmp_path):
    """`uint64_t` storage is an explicitly unsupported width (module
    docstring): introducing one must hard-fail, never silently truncate."""
    fsp_dir = _copy_fixture(tmp_path)
    iodefine = fsp_dir / "iodefines" / "gpio_iodefine.h"
    text = iodefine.read_text()
    assert "__IM uint8_t RESERVED[4];" in text
    mutated = text.replace(
        "__IM uint8_t RESERVED[4];",
        "__IM uint8_t RESERVED[4];\n    __IOM uint64_t GHOST64;",
    )
    assert mutated != text
    iodefine.write_text(mutated)

    proc = _run(fsp_dir, "--check")
    assert proc.returncode != 0
    assert "uint64_t" in proc.stderr


def test_a_uint64_bitfield_is_rejected(tmp_path):
    fsp_dir = _copy_fixture(tmp_path)
    iodefine = fsp_dir / "iodefines" / "gpio_iodefine.h"
    text = iodefine.read_text()
    marker = "            __IOM uint8_t PSM : 2;\n"
    assert marker in text
    mutated = text.replace(marker, marker + "            __IOM uint64_t GHOST : 3;\n")
    assert mutated != text
    iodefine.write_text(mutated)

    proc = _run(fsp_dir, "--check")
    assert proc.returncode != 0
    assert "uint64_t" in proc.stderr


def test_a_deleted_pad_bit_is_rejected_by_the_width_sum_check(tmp_path):
    """Delete the anonymous `: 1` pad from ELC_PEL_b (PSB:3 + PSP:2 +
    PSM:2 = 7, one short of the register's 8 bits) -- this is the
    bitfield-width-sum check in `parse_union` (SPDCR2/TTRG's struct was
    WRONG but still summed to 32 bits, so this check did not catch it; a
    MISSING pad, unlike a mis-sized one, is exactly what this check does
    catch). Confirmed RED-then-green while developing this test: with the
    `chosen and lsb != total_bits` comparison in `parse_union` neutered
    (hand-edited to `if False:`), this test itself fails -- restored
    before landing, not asserted here since that would require editing
    the generator from the test."""
    fsp_dir = _copy_fixture(tmp_path)
    iodefine = fsp_dir / "iodefines" / "gpio_iodefine.h"
    text = iodefine.read_text()
    marker = "            uint8_t           : 1;\n"
    assert marker in text
    mutated = text.replace(marker, "", 1)
    assert mutated != text
    iodefine.write_text(mutated)

    proc = _run(fsp_dir, "--check")
    assert proc.returncode != 0
    assert "ELC_PEL" in proc.stderr
    assert "bitfield widths sum to 7 bits" in proc.stderr


def test_a_perturbed_size_hint_comment_is_rejected(tmp_path):
    """Mutate the fixture's `/*!< Size = 4 (0x4) */` hint on
    R_ELC_PDBF_Type's closing brace to a wrong value -- the size-hint
    cross-check in `expand_type` must reject it (this is the mechanism
    SIZE_HINT_SKIPS deliberately silences for R_CANFD_Type in the real
    corpus; here it must still fire for a type NOT in that skip set).
    Confirmed RED-then-green while developing this test: with the
    `hint is not None and hint != offset` comparison in `expand_type`
    neutered (hand-edited to `if False:`), this test itself fails --
    restored before landing, not asserted here since that would require
    editing the generator from the test."""
    fsp_dir = _copy_fixture(tmp_path)
    iodefine = fsp_dir / "iodefines" / "gpio_iodefine.h"
    text = iodefine.read_text()
    marker = "} R_ELC_PDBF_Type; /*!< Size = 4 (0x4) */"
    assert marker in text
    mutated = text.replace(marker, "} R_ELC_PDBF_Type; /*!< Size = 5 (0x5) */")
    assert mutated != text
    iodefine.write_text(mutated)

    proc = _run(fsp_dir, "--check")
    assert proc.returncode != 0
    assert "R_ELC_PDBF_Type" in proc.stderr
    assert "computed size" in proc.stderr


def test_field_skip_used_only_when_it_actually_suppresses_a_problem(monkeypatch):
    """`cross_check_file`'s `used_field_skips` return value must reflect
    whether a FIELD_CROSS_CHECK_SKIPS entry ACTUALLY suppressed a real
    disagreement, not merely whether its (stem, regname, fname) key was
    present in `field_records` -- recording it as "used" just for being
    looked up is the exact gap that let 32 dead dmac_b/DSTAT_* entries
    accumulate unnoticed against the real corpus (issue #1029 step 2
    review), because generate()'s "unused skip-list entry" strictness
    (which IS covered end-to-end by the other tests here) can only catch
    what this function correctly reports as unused in the first place.

    Calls `cross_check_file` directly, at the unit level, with a
    synthetic single-field/single-macro corpus -- bypassing `generate()`'s
    whole-26-file-corpus strictness gate entirely avoids a confound where
    every OTHER real skip-list entry would also read as "unused" against
    a small fixture, for reasons unrelated to the one entry under test."""
    import gen_rzv2n_cm33_svd as g

    bitmasks = {"R_STEM_REG_FIELD": (0, 0xF)}  # pos=0, width=4
    monkeypatch.setitem(g.FIELD_CROSS_CHECK_SKIPS, ("stem", "REG", "FIELD"), "dead entry for this test")

    # The field genuinely AGREES with its iobitmask macro (lsb=0 width=4,
    # matching pos=0/msk=0xF above) -- the skip suppresses nothing and
    # must NOT be reported as used.
    _, used, _, _ = g.cross_check_file("stem", [("REG", "FIELD", 0, 4)], bitmasks)
    assert used == set()

    # The SAME skip key, but now a genuine disagreement (lsb=1, not 0) --
    # the skip suppresses a real `problems.append` and IS used.
    _, used, _, _ = g.cross_check_file("stem", [("REG", "FIELD", 1, 4)], bitmasks)
    assert used == {("stem", "REG", "FIELD")}


def test_field_position_override_no_longer_corroborated_is_rejected():
    """FIELD_POSITION_OVERRIDES is a claim that the iobitmask side agrees
    with the override value -- that claim must be RE-checked on every run,
    not trusted forever once a review comment justified it (issue #1029
    step 2 second review: an unfalsifiable override is the same defect
    class the whole cross-check exists to catch). Uses the REAL production
    spi_b/SPDCR2/TTRG entry ((8, 4)) with a synthetic iobitmask macro whose
    _Pos has drifted to 12 -- the struct-derived (lsb=10, width=4) is
    unchanged, only the iobitmask side moved. Confirmed RED-then-green
    while developing this test: with the override re-corroboration check
    in `cross_check_file` neutered (hand-edited to skip the `(pos,
    bit_width) != override` comparison), this test itself fails -- restored
    before landing, not asserted here since that would require editing the
    generator from the test."""
    assert g.FIELD_POSITION_OVERRIDES[("spi_b", "SPDCR2", "TTRG")] == (8, 4)
    bitmasks = {"R_SPI_B0_SPDCR2_TTRG": (12, 0xF000)}  # was (8, 0xF00)
    with pytest.raises(g.SvdGenError, match="no longer agrees"):
        g.cross_check_file("spi_b", [("SPDCR2", "TTRG", 10, 4)], bitmasks)


def test_field_position_override_still_corroborated_is_accepted():
    """The corroboration check must not false-positive on the real,
    unmutated evidence: spi_b/SPDCR2/TTRG's real iobitmask macro
    (pos=8, width=4) still agrees with the override."""
    bitmasks = {"R_SPI_B0_SPDCR2_TTRG": (8, 0xF00)}
    problems, _, _, used_overrides = g.cross_check_file(
        "spi_b", [("SPDCR2", "TTRG", 10, 4)], bitmasks
    )
    assert problems == []
    assert used_overrides == {("spi_b", "SPDCR2", "TTRG")}


def test_unused_field_position_override_is_rejected(monkeypatch, tmp_path):
    """A FIELD_POSITION_OVERRIDES entry that never matches a real field is
    a dead constant, exactly like the other two skip tables -- generate()
    must reject it under the same real-corpus-file-count gate. Lowers
    REAL_IODEFINE_FILE_COUNT to the fixture's own file count (1) so the
    strictness gate fires against this small corpus, and clears the OTHER
    two skip tables first (the real entries in them are unreachable from
    this gpio-only fixture and would otherwise fail the check earlier, for
    a reason unrelated to the one entry under test here)."""
    fsp_dir = _copy_fixture(tmp_path)
    monkeypatch.setattr(g, "REAL_IODEFINE_FILE_COUNT", 1)
    monkeypatch.setattr(g, "FIELD_CROSS_CHECK_SKIPS", {})
    monkeypatch.setattr(g, "IOBITMASK_ORPHAN_SKIPS", {})
    monkeypatch.setitem(g.FIELD_POSITION_OVERRIDES, ("nosuch", "NOREG", "NOFIELD"), (3, 1))
    with pytest.raises(g.SvdGenError, match="FIELD_POSITION_OVERRIDES entries never matched"):
        g.generate(fsp_dir)


def test_unused_size_hint_skip_is_rejected(monkeypatch, tmp_path):
    """SIZE_HINT_SKIPS entries must also be caught when dead -- if Renesas
    corrects a stale `Size = N` comment, the skip should stop being needed
    and generate() must say so instead of the entry living forever. Clears
    the other three skip/override tables first for the same reason as
    above: their real entries are unreachable from this gpio-only fixture."""
    fsp_dir = _copy_fixture(tmp_path)
    monkeypatch.setattr(g, "REAL_IODEFINE_FILE_COUNT", 1)
    monkeypatch.setattr(g, "FIELD_CROSS_CHECK_SKIPS", {})
    monkeypatch.setattr(g, "IOBITMASK_ORPHAN_SKIPS", {})
    monkeypatch.setattr(g, "FIELD_POSITION_OVERRIDES", {})
    monkeypatch.setattr(g, "SIZE_HINT_SKIPS", g.SIZE_HINT_SKIPS | {"R_NO_SUCH_Type"})
    with pytest.raises(g.SvdGenError, match="SIZE_HINT_SKIPS entries never suppressed"):
        g.generate(fsp_dir)


def test_cluster_colliding_with_a_register_offset_hard_fails():
    """`assign_alternate_registers`'s own docstring promises a
    cluster/register offset collision is a hard fail, not a silent
    alternateRegister-of-None -- not reachable via the real parser today
    (Overlay never carries a ClusterMember), but the promise itself must
    hold when called directly."""
    cluster = g.SvdCluster(name="CL", offset=0, dim=None, dim_increment=4, children=[])
    reg = g.SvdRegister(name="REG", offset=0, elem_bits=32, access="read-write", fields=[], dim=None)
    with pytest.raises(g.SvdGenError, match="cluster collides"):
        g.assign_alternate_registers([cluster, reg], "ctx")


# --- unit-level checks on the smaller helpers -------------------------------


def test_mask_width_rejects_non_contiguous_mask():
    with pytest.raises(g.SvdGenError, match="not a single contiguous"):
        g.mask_width(0b1011, "R_TEST_MACRO", "ctx")


def test_mask_width_computes_pos_independent_width():
    # 0x40 = bit 6 only -- a regression fixture for the trailing-zero-count
    # bug this generator's own development hit (naive width-from-bit-0
    # counting silently returned 0 for any mask not covering bit 0).
    assert g.mask_width(0x40, "R_TEST_MACRO", "ctx") == 1


def test_find_macro_falls_back_to_stripped_leading_token():
    bitmasks = {"R_TSU_B0_SSUSR_EN_TS": (0, 1)}
    # Exact match first.
    assert g._find_macro(bitmasks, "SSUSR", "EN_TS") == ["R_TSU_B0_SSUSR_EN_TS"]
    # Falls back when the iodefine register's own name repeats a leading
    # token ("TSU_") the iobitmask macro does not.
    assert g._find_macro(bitmasks, "TSU_SSUSR", "EN_TS") == ["R_TSU_B0_SSUSR_EN_TS"]


def test_find_macro_returns_nothing_for_a_true_miss():
    assert g._find_macro({"R_X_Y_Z": (0, 1)}, "NOPE", "NOPE") == []
