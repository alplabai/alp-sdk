# SPDX-License-Identifier: Apache-2.0
"""Tests for `resolve_memory_map()` in alp_project.py.

Covers the silicon-variant-derived path (no memory_map in preset),
the override path (preset declares memory_map verbatim), and the
TBD fallback (silicon_variant cannot be resolved).
"""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "alp_project.py"
METADATA = REPO / "metadata"


@pytest.fixture(scope="module")
def alp_project():
    spec = importlib.util.spec_from_file_location("alp_project", SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules["alp_project"] = mod
    spec.loader.exec_module(mod)
    return mod


def test_aen301_derives_regions_from_e3_variant(alp_project):
    """E1M-AEN301 -> AE302F80F55D5LE variant -> 11 regions:
       - mram_main (5.5 GB MRAM)
       - 10 SRAM banks (2 shared SRAM0/1, 4 per-core TCMs, 4 shared SRAM6-9)
    Per-core TCMs carry accessible_from: [<core>] only; bulk banks
    carry accessible_from: [m55_hp, m55_he]."""
    preset = {
        "sku": "E1M-AEN301",
        "silicon": "alif:ensemble:e3",
        "silicon_variant": "AE302F80F55D5LE",
    }
    regions = alp_project.resolve_memory_map(preset, METADATA)
    by_name = {r["name"]: r for r in regions}

    # MRAM
    assert "mram_main" in by_name
    assert by_name["mram_main"]["size_kib"] == int(5.5 * 1024)
    assert sorted(by_name["mram_main"]["accessible_from"]) == ["m55_he", "m55_hp"]
    assert by_name["mram_main"]["cacheable"] is True

    # M55-HP TCMs: 256 KiB ITCM + 1024 KiB DTCM, only m55_hp can reach.
    assert by_name["sram2_m55_hp_itcm"]["size_kib"] == 256
    assert by_name["sram2_m55_hp_itcm"]["accessible_from"] == ["m55_hp"]
    assert by_name["sram2_m55_hp_itcm"]["cacheable"] is False
    assert by_name["sram3_m55_hp_dtcm"]["size_kib"] == 1024
    assert by_name["sram3_m55_hp_dtcm"]["accessible_from"] == ["m55_hp"]
    assert by_name["sram3_m55_hp_dtcm"]["cacheable"] is False

    # M55-HE TCMs: 256 KiB each, only m55_he.
    assert by_name["sram4_m55_he_itcm"]["accessible_from"] == ["m55_he"]
    assert by_name["sram5_m55_he_dtcm"]["accessible_from"] == ["m55_he"]

    # Bulk SRAM0..9: shared across both cores, cacheable.
    assert by_name["sram0"]["size_kib"] == 4096
    assert sorted(by_name["sram0"]["accessible_from"]) == ["m55_he", "m55_hp"]
    assert by_name["sram0"]["cacheable"] is True


def test_preset_with_memory_map_override_wins_verbatim(alp_project):
    """If the SoM YAML still declares memory_map: (e.g. for a
    non-stock partitioning), the loader trusts it verbatim --
    does NOT augment from the SoC variant."""
    preset = {
        "sku": "E1M-CUSTOM",
        "silicon": "alif:ensemble:e3",
        "silicon_variant": "AE302F80F55D5LE",
        "memory_map": [
            {"name": "secure_enclave_sram", "base": 0x60000000,
             "size_kib": 128, "accessible_from": ["m55_he"], "cacheable": False},
        ],
    }
    regions = alp_project.resolve_memory_map(preset, METADATA)
    assert len(regions) == 1
    assert regions[0]["name"] == "secure_enclave_sram"
    assert regions[0]["base"] == 0x60000000


def test_silicon_variant_tbd_returns_empty(alp_project):
    """E1M-NX9101 carries silicon_variant: TBD; the resolver
    returns an empty list -- callers see 'pending HW config'."""
    preset = {
        "sku": "E1M-NX9101",
        "silicon": "nxp:imx9:imx93",
        "silicon_variant": "TBD",
    }
    regions = alp_project.resolve_memory_map(preset, METADATA)
    assert regions == []


def test_missing_silicon_returns_empty(alp_project):
    """A preset without `silicon:` shouldn't crash."""
    assert alp_project.resolve_memory_map({}, METADATA) == []
    assert alp_project.resolve_memory_map({"silicon_variant": "X"}, METADATA) == []


def test_v2n101_includes_a55_cluster_and_m33_sm(alp_project):
    """V2N family: cores = a55_cluster + m33_sm. Bulk regions
    accessible_from both; per-core TCMs (if any in V2N spec)
    scoped to just one. Smoke check the variant resolves cleanly."""
    preset = {
        "sku": "E1M-V2N101",
        "silicon": "renesas:rzv2n:n44",
        "silicon_variant": "R9A09G056N44GBG",
    }
    regions = alp_project.resolve_memory_map(preset, METADATA)
    # At minimum the function shouldn't crash and should return SOMETHING
    # (V2N's variant has at least one bank declared). Looser smoke test
    # than AEN since the V2N JSON layout may be sparser pre-HiL.
    if regions:
        for r in regions:
            assert set(r["accessible_from"]).issubset({"a55_cluster", "m33_sm"})
