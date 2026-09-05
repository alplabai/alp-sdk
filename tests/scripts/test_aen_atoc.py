# SPDX-License-Identifier: Apache-2.0
"""Tests for the #1069 ATOC-assembly guard in scripts/aen_atoc.py."""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "aen_atoc.py"


@pytest.fixture(scope="module")
def aen_atoc():
    spec = importlib.util.spec_from_file_location("aen_atoc", SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules["aen_atoc"] = mod
    spec.loader.exec_module(mod)
    return mod


def test_disjoint_he_hp_entries_pass(aen_atoc):
    """The #1069 fix: HE and HP each in their own window -- no collision."""
    aen_atoc.validate_atoc_entries({
        "DEVICE": {"disabled": False, "binary": "app-device-config.json"},
        "ALP-HE": {"cpu_id": "M55_HE", "mramAddress": "0x80010000", "flags": ["boot"]},
        "ALP-HP": {"cpu_id": "M55_HP", "mramAddress": "0x802b0000", "flags": ["boot"]},
    })


def test_original_bug_both_at_0x80010000_rejected(aen_atoc):
    """The pre-#1069 collision (both cores' images resolved to the shared
    0x80010000 window) is now caught as an out-of-window HP address --
    HP's window moved to 0x802b0000, so the historical bug address no
    longer belongs to it at all."""
    with pytest.raises(aen_atoc.AtocValidationError, match="outside the M55_HP slot0 window"):
        aen_atoc.validate_atoc_entries({
            "ALP-HE": {"cpu_id": "M55_HE", "mramAddress": "0x80010000", "flags": ["boot"]},
            "ALP-HP": {"cpu_id": "M55_HP", "mramAddress": "0x80010000", "flags": ["boot"]},
        })


def test_same_address_same_window_still_caught_as_collision(aen_atoc):
    """Independent of the window check: two entries that DO land in the
    same declared window at the identical address are rejected as a
    same-address collision (defence-in-depth if a future bug ever makes
    two entries resolve the same cpu_id)."""
    with pytest.raises(aen_atoc.AtocValidationError, match="same mramAddress"):
        aen_atoc.validate_atoc_entries({
            "ALP-HE-1": {"cpu_id": "M55_HE", "mramAddress": "0x80010000", "flags": ["boot"]},
            "ALP-HE-2": {"cpu_id": "M55_HE", "mramAddress": "0x80010000", "flags": ["boot"]},
        })


def test_address_outside_own_window_rejected(aen_atoc):
    """HP staged into HE's window (not just any collision -- a plain
    out-of-window address) is also caught."""
    with pytest.raises(aen_atoc.AtocValidationError, match="outside the M55_HP slot0 window"):
        aen_atoc.validate_atoc_entries({
            "ALP-HP": {"cpu_id": "M55_HP", "mramAddress": "0x80010000", "flags": ["boot"]},
        })


def test_address_past_mram_end_rejected(aen_atoc):
    with pytest.raises(aen_atoc.AtocValidationError, match="outside System MRAM"):
        aen_atoc.validate_atoc_entries({
            "ALP-HE": {"cpu_id": "M55_HE", "mramAddress": "0x80580000", "flags": ["boot"]},
        })


def test_itcm_loadaddress_entries_are_not_window_checked(aen_atoc):
    """The current AEN dual-core examples stage both cores at ITCM
    loadAddress values -- this guard must not touch those (#1069 PR body:
    'must not reject the ITCM dual-core case')."""
    aen_atoc.validate_atoc_entries({
        "ALP-HP": {"cpu_id": "M55_HP", "loadAddress": "0x50000000", "flags": ["load", "boot"]},
        "ALP-HE": {"cpu_id": "M55_HE", "loadAddress": "0x58000000",
                   "flags": ["load", "boot", "deferred"]},
    })


def test_a32_boot_config_accepted(aen_atoc):
    """#1981: the A32 Linux chain measured booting on E1M-AEN803 --
    BOOTLOAD (TF-A SP_MIN, XIP at 0x80002000) + A32_APP (xipImage at
    0x80020000), both cpu_id A32_0 -- must be accepted. Before the fix
    the guard rejected it as an 'unknown cpu_id'."""
    aen_atoc.validate_atoc_entries({
        "DEVICE": {"disabled": False, "binary": "app-device-config.json"},
        "BOOTLOAD": {"cpu_id": "A32_0", "mramAddress": "0x80002000", "flags": ["boot"]},
        "A32_APP": {"cpu_id": "A32_0", "mramAddress": "0x80020000", "flags": []},
    })


def test_a32_and_m55_slot0_entries_rejected(aen_atoc):
    """The A32 chain spans both M55 slot0 windows, so an M55 slot0-XIP
    entry alongside an A32 mramAddress entry is a layout the SE cannot
    boot -- mutually exclusive, not merely overlapping."""
    with pytest.raises(aen_atoc.AtocValidationError, match="mutually exclusive"):
        aen_atoc.validate_atoc_entries({
            "A32_APP": {"cpu_id": "A32_0", "mramAddress": "0x80020000", "flags": []},
            "ALP-HP": {"cpu_id": "M55_HP", "mramAddress": "0x802b0000", "flags": ["boot"]},
        })


def test_a32_config_with_itcm_m55_stubs_accepted(aen_atoc):
    """The M55 entries of a real A32 ATOC are ITCM stubs (HP 0x50000000
    / HE 0x58000000, per the measured SE boot table) -- loadAddress
    entries, so the exclusivity rule must not trip on them."""
    aen_atoc.validate_atoc_entries({
        "BOOTLOAD": {"cpu_id": "A32_0", "mramAddress": "0x80002000", "flags": ["boot"]},
        "A32_APP": {"cpu_id": "A32_0", "mramAddress": "0x80020000", "flags": []},
        "HP_APP": {"cpu_id": "M55_HP", "loadAddress": "0x50000000",
                   "flags": ["load", "boot"]},
        "HE_APP": {"cpu_id": "M55_HE", "loadAddress": "0x58000000",
                   "flags": ["load", "boot"]},
    })


def test_a32_address_below_bl32_base_rejected(aen_atoc):
    """0x80001000 is inside System MRAM but below the A32 region base
    0x80002000 (the TF-A BL32 XIP base), so it is out of window. The
    region's TOP is not checked here -- it is the per-build APP Package
    start, not a constant (see the module docstring)."""
    with pytest.raises(aen_atoc.AtocValidationError, match="below the A32_0 MRAM region"):
        aen_atoc.validate_atoc_entries({
            "BOOTLOAD": {"cpu_id": "A32_0", "mramAddress": "0x80001000", "flags": ["boot"]},
        })


def test_validate_atoc_config_file(tmp_path, aen_atoc):
    cfg = tmp_path / "dualcore.json"
    cfg.write_text(
        '{"ALP-HE": {"cpu_id": "M55_HE", "mramAddress": "0x80010000", '
        '"flags": ["boot"]}}',
        encoding="utf-8")
    aen_atoc.validate_atoc_config_file(cfg)  # doesn't raise


def test_cli_main_rejects_and_exits_nonzero(tmp_path, aen_atoc):
    cfg = tmp_path / "bad.json"
    cfg.write_text(
        '{"ALP-HE": {"cpu_id": "M55_HE", "mramAddress": "0x80010000", "flags": ["boot"]}, '
        ' "ALP-HP": {"cpu_id": "M55_HP", "mramAddress": "0x80010000", "flags": ["boot"]}}',
        encoding="utf-8")
    assert aen_atoc.main([str(cfg)]) == 1
