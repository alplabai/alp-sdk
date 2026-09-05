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


def test_validate_atoc_config_file(tmp_path, aen_atoc):
    cfg = tmp_path / "dualcore.json"
    cfg.write_text(
        '{"ALP-HE": {"cpu_id": "M55_HE", "mramAddress": "0x80010000", '
        '"flags": ["boot"]}}',
        encoding="utf-8")
    aen_atoc.validate_atoc_config_file(cfg)  # doesn't raise


def test_a32_linux_boot_config_passes(aen_atoc):
    """#1981: the exact four-entry config from the issue -- TF-A BL32
    BOOTLOAD + the kernel A32_APP at their bench-verified mramAddress
    values, alongside the M55 HP/HE stub entries at their ITCM
    loadAddress values -- must validate. This is the configuration
    the Secure Enclave's own boot table reported as booted on-silicon
    (E1M-AEN803, 2026-09-05): BOOTLOAD cpu_id A32_0, boot addr
    0x80002000, flags 'u VB'."""
    aen_atoc.validate_atoc_entries({
        "BOOTLOAD": {"binary": "bl32.bin", "mramAddress": "0x80002000",
                     "cpu_id": "A32_0", "flags": ["boot"], "signed": True},
        "A32_APP": {"binary": "xipImage.bin", "mramAddress": "0x80020000",
                    "cpu_id": "A32_0", "signed": True},
        "HP_APP": {"binary": "m55_stub_hp.bin", "loadAddress": "0x50000000",
                   "cpu_id": "M55_HP", "flags": ["load", "boot"], "signed": True},
        "HE_APP": {"binary": "m55_stub_he.bin", "loadAddress": "0x58000000",
                   "cpu_id": "M55_HE", "flags": ["load", "boot"], "signed": True},
    })


def test_a32_address_below_window_floor_rejected(aen_atoc):
    """A genuinely out-of-window A32 address (below the bench-verified
    BOOTLOAD floor of 0x80002000, but still inside System MRAM) is
    still caught -- the new A32_0 window isn't a rubber stamp."""
    with pytest.raises(aen_atoc.AtocValidationError, match="outside the A32_0 slot0 window"):
        aen_atoc.validate_atoc_entries({
            "BOOTLOAD": {"binary": "bl32.bin", "mramAddress": "0x80001000",
                         "cpu_id": "A32_0", "flags": ["boot"], "signed": True},
        })


def test_a32_and_m55_mramaddress_mix_rejected(aen_atoc):
    """#1981's mutual-exclusivity invariant: an A32_0 mramAddress entry
    and an M55 mramAddress entry (not loadAddress -- that stays exempt)
    must never coexist, since the A32 window covers the whole M55
    window range."""
    with pytest.raises(aen_atoc.AtocValidationError, match="mixes an A32 mramAddress entry"):
        aen_atoc.validate_atoc_entries({
            "BOOTLOAD": {"binary": "bl32.bin", "mramAddress": "0x80002000",
                         "cpu_id": "A32_0", "flags": ["boot"], "signed": True},
            "ALP-HE": {"cpu_id": "M55_HE", "mramAddress": "0x80010000", "flags": ["boot"]},
        })


def test_a32_address_at_atoc_band_rejected(aen_atoc):
    """Round-12b review: the A32_0 ceiling must stop at the base of the
    SE-owned `atoc` band (metadata/e1m_modules/E1M-AEN801.yaml:274 /
    E1M-AEN803.yaml:201, both `{ name: atoc, base: 0x80578000,
    size_kib: 32 }`), not the raw System MRAM end (0x80580000) -- the
    original #1981 landing hardcoded the latter, which silently accepted
    an A32 mramAddress entry anywhere inside that 32 KiB SE boot-table
    band. 0x8057c000 sits inside it (0x80578000 + 0x4000)."""
    with pytest.raises(aen_atoc.AtocValidationError, match="outside the A32_0 slot0 window"):
        aen_atoc.validate_atoc_entries({
            "BOOTLOAD": {"binary": "bl32.bin", "mramAddress": "0x8057c000",
                         "cpu_id": "A32_0", "flags": ["boot"], "signed": True},
        })


def test_a32_address_exactly_at_atoc_band_base_rejected(aen_atoc):
    """The atoc band's own base (0x80578000) is the first byte the SE
    owns -- the window's exclusive upper edge, same convention as the
    M55 windows' own top-exclusive bound."""
    with pytest.raises(aen_atoc.AtocValidationError, match="outside the A32_0 slot0 window"):
        aen_atoc.validate_atoc_entries({
            "BOOTLOAD": {"binary": "bl32.bin", "mramAddress": "0x80578000",
                         "cpu_id": "A32_0", "flags": ["boot"], "signed": True},
        })


def test_a32_address_near_mram_end_rejected(aen_atoc):
    """0x8057ffff -- one byte below System MRAM end, still inside the
    SE-owned atoc band -- is caught by the A32_0 window itself, not just
    the separate System-MRAM bound (which alone would have let it
    through: MRAM_END was the old, too-wide A32_0 ceiling)."""
    with pytest.raises(aen_atoc.AtocValidationError, match="outside the A32_0 slot0 window"):
        aen_atoc.validate_atoc_entries({
            "BOOTLOAD": {"binary": "bl32.bin", "mramAddress": "0x8057ffff",
                         "cpu_id": "A32_0", "flags": ["boot"], "signed": True},
        })


@pytest.mark.parametrize("address", [
    "0x80010000",  # the A32 chain's own device tree, per #1981's shipped
                   # layout -- numerically the same as the M55_HE slot0
                   # base, but not concurrently staged in one config
    "0x802b0000",  # M55_HP slot0 base -- same
    "0x80300000",  # mid M55_HP window
    "0x80380000",  # cramfs root (#1981), the highest real A32 mramAddress
                   # in the issue's shipped layout
    "0x80550000",  # `reserved` band base
    "0x80560000",  # `storage` band base
])
def test_a32_pure_config_accepts_addresses_below_atoc_band(aen_atoc, address):
    """A pure A32-only config (no M55 mramAddress entry present, so the
    mutual-exclusivity check never fires) is legitimate anywhere below
    the atoc band, even at addresses that numerically coincide with an
    M55 slot0 base -- those only matter when an M55 mramAddress entry is
    ALSO staged (see test_a32_and_m55_mramaddress_mix_rejected). The
    window -- not the mutual-exclusivity check -- is what accepts these;
    this is the "make the window carry that weight" half of the fix."""
    aen_atoc.validate_atoc_entries({
        "A32_APP": {"binary": "xipImage.bin", "mramAddress": address,
                    "cpu_id": "A32_0", "signed": True},
    })


def test_a32_1_membership_does_not_trip_m55_mutual_exclusion(aen_atoc, monkeypatch):
    """The mutual-exclusivity check used to test `cpu_id != 'A32_0'` to
    decide "is this an M55 entry" -- which mislabels any future second
    A32-cluster key (the Alif E8's A32 cluster is dual-core) as an M55
    entry and wrongly trips mutual exclusion against A32_0. Simulate that
    key with monkeypatch (nothing in this codebase declares A32_1 yet)
    and confirm two same-cluster A32 entries never trip the M55
    mutual-exclusivity guard."""
    monkeypatch.setitem(aen_atoc.SLOT0_WINDOWS, "A32_1",
                         aen_atoc.SLOT0_WINDOWS["A32_0"])
    aen_atoc.validate_atoc_entries({
        "BOOTLOAD": {"mramAddress": "0x80002000", "cpu_id": "A32_0",
                     "flags": ["boot"], "signed": True},
        "A32_1_APP": {"mramAddress": "0x80020000", "cpu_id": "A32_1",
                      "signed": True},
    })


def test_cli_main_rejects_and_exits_nonzero(tmp_path, aen_atoc):
    cfg = tmp_path / "bad.json"
    cfg.write_text(
        '{"ALP-HE": {"cpu_id": "M55_HE", "mramAddress": "0x80010000", "flags": ["boot"]}, '
        ' "ALP-HP": {"cpu_id": "M55_HP", "mramAddress": "0x80010000", "flags": ["boot"]}}',
        encoding="utf-8")
    assert aen_atoc.main([str(cfg)]) == 1
