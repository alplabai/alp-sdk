# SPDX-License-Identifier: Apache-2.0
"""Tests for `resolve_capabilities()` in alp_project.py.

Covers:
  - Happy-path silicon merge (SoC side keys present in result).
  - SoM override wins over SoC default on same key.
  - SoM-only extension keys (e.g. optiga_trust_m, tmu_cordic) survive
    the merge unchanged.
  - Missing silicon ref returns only SoM-side caps (no crash).
  - Both sides absent returns empty dict.

Uses synthetic metadata roots (tmp_path fixture) to avoid coupling to
SoM YAML files that sibling agents may be editing in parallel.  One
smoke test also loads the real V2N101 SoC JSON directly to exercise
the full path.
"""
from __future__ import annotations

import importlib.util
import json
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


# ---------------------------------------------------------------------------
# Synthetic metadata root helpers (mirroring test_resolve_memory_map pattern)
# ---------------------------------------------------------------------------

def _make_soc_json(tmp_path: Path, vendor: str, family: str, part: str,
                   capabilities: dict) -> Path:
    """Write a minimal SoC JSON with the given capabilities block."""
    soc_dir = tmp_path / "socs" / vendor / family
    soc_dir.mkdir(parents=True, exist_ok=True)
    soc_file = soc_dir / f"{part}.json"
    soc_data = {
        "soc_spec_version": 1,
        "ref": f"{vendor}:{family}:{part}",
        "vendor": vendor,
        "family": family,
        "part": part,
        "cores": [{"id": "m55_hp", "type": "cortex-m55", "count": 1}],
        "npus": [],
        "peripherals": {},
        "capabilities": capabilities,
    }
    soc_file.write_text(json.dumps(soc_data), encoding="utf-8")
    return tmp_path


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_soc_caps_appear_in_merged_result(alp_project, tmp_path):
    """SoC-side keys are present in the merged result when SoM has no caps."""
    soc_caps = {"drp_ai": True, "neon": True, "helium_mve": False}
    meta = _make_soc_json(tmp_path, "testvendor", "testfam", "testpart", soc_caps)

    preset = {
        "sku": "E1M-TEST",
        "silicon": "testvendor:testfam:testpart",
    }
    result = alp_project.resolve_capabilities(preset, meta)

    assert result["drp_ai"] is True
    assert result["neon"] is True
    assert result["helium_mve"] is False


def test_som_override_wins_over_soc_default(alp_project, tmp_path):
    """SoM-declared value wins over the SoC default for the same key."""
    soc_caps = {"cau": False, "neon": True}
    meta = _make_soc_json(tmp_path, "testvendor", "testfam", "testpart", soc_caps)

    preset = {
        "sku": "E1M-TEST",
        "silicon": "testvendor:testfam:testpart",
        # SoM says cau is available via bridge
        "capabilities": {"cau": True},
    }
    result = alp_project.resolve_capabilities(preset, meta)

    # SoM wins
    assert result["cau"] is True
    # SoC key still present when SoM doesn't override it
    assert result["neon"] is True


def test_som_only_extension_keys_survive(alp_project, tmp_path):
    """SoM-only keys (e.g. optiga_trust_m, tmu_cordic) survive the merge
    even when the SoC JSON has no such key."""
    soc_caps = {"drp_ai": True}
    meta = _make_soc_json(tmp_path, "testvendor", "testfam", "testpart", soc_caps)

    preset = {
        "sku": "E1M-TEST",
        "silicon": "testvendor:testfam:testpart",
        "capabilities": {
            "optiga_trust_m": True,
            "tmu_cordic": True,
            "tmu_fft": True,
            "tmu_fac": True,
        },
    }
    result = alp_project.resolve_capabilities(preset, meta)

    assert result["optiga_trust_m"] is True
    assert result["tmu_cordic"] is True
    assert result["tmu_fft"] is True
    assert result["tmu_fac"] is True
    # SoC key also present
    assert result["drp_ai"] is True


def test_missing_silicon_returns_only_som_caps(alp_project, tmp_path):
    """Preset without `silicon:` returns SoM caps only (no crash)."""
    preset = {
        "sku": "E1M-TEST",
        "capabilities": {"optiga_trust_m": True},
    }
    result = alp_project.resolve_capabilities(preset, tmp_path)
    assert result == {"optiga_trust_m": True}


def test_both_sides_absent_returns_empty(alp_project, tmp_path):
    """Preset with no silicon and no capabilities returns {}."""
    result = alp_project.resolve_capabilities({}, tmp_path)
    assert result == {}


def test_unknown_silicon_ref_returns_only_som_caps(alp_project, tmp_path):
    """A silicon ref that doesn't resolve to a file returns only SoM caps."""
    preset = {
        "sku": "E1M-TEST",
        "silicon": "unknown:vendor:part",
        "capabilities": {"custom_key": True},
    }
    result = alp_project.resolve_capabilities(preset, tmp_path)
    assert result == {"custom_key": True}


def test_unpopulated_restriction_forces_flag_false(alp_project, tmp_path):
    """`silicon_capabilities.unpopulated` forces a truthy silicon flag to False."""
    soc_caps = {"gpu2d": True, "dave2d": True, "neon": True}
    meta = _make_soc_json(tmp_path, "testvendor", "testfam", "testpart", soc_caps)

    preset = {
        "sku": "E1M-TEST",
        "silicon": "testvendor:testfam:testpart",
        "silicon_capabilities": {"unpopulated": ["gpu2d", "dave2d"]},
    }
    result = alp_project.resolve_capabilities(preset, meta)

    assert result["gpu2d"] is False
    assert result["dave2d"] is False
    # Unlisted silicon caps keep the full silicon value.
    assert result["neon"] is True


def test_unpopulated_restriction_forces_count_to_zero(alp_project, tmp_path):
    """Count-style silicon caps (e.g. ethos_u55_count) restrict to 0, not False,
    so `> 0` presence checks downstream keep their integer semantics."""
    soc_caps = {"ethos_u55_count": 2, "ethos_u85_count": 1, "helium_mve": True}
    meta = _make_soc_json(tmp_path, "testvendor", "testfam", "testpart", soc_caps)

    preset = {
        "sku": "E1M-TEST",
        "silicon": "testvendor:testfam:testpart",
        "silicon_capabilities": {"unpopulated": ["ethos_u85_count"]},
    }
    result = alp_project.resolve_capabilities(preset, meta)

    assert result["ethos_u85_count"] == 0
    assert isinstance(result["ethos_u85_count"], int)
    assert result["ethos_u55_count"] == 2
    assert result["helium_mve"] is True


def test_no_restriction_field_keeps_full_silicon_set(alp_project, tmp_path):
    """Absence of `silicon_capabilities:` = full silicon capability set
    (the zero-behaviour-change default for every existing SKU)."""
    soc_caps = {"gpu2d": True, "neon": True}
    meta = _make_soc_json(tmp_path, "testvendor", "testfam", "testpart", soc_caps)

    preset = {"sku": "E1M-TEST", "silicon": "testvendor:testfam:testpart"}
    restricted = dict(preset, silicon_capabilities={"unpopulated": ["gpu2d"]})

    assert alp_project.resolve_capabilities(preset, meta) == {
        "gpu2d": True, "neon": True}
    assert alp_project.resolve_capabilities(restricted, meta) == {
        "gpu2d": False, "neon": True}


def test_som_unpopulated_capabilities_accessor(alp_project):
    """The shared accessor returns [] for absent/malformed blocks and the
    transcribed list otherwise."""
    assert alp_project.som_unpopulated_capabilities({}) == []
    assert alp_project.som_unpopulated_capabilities(
        {"silicon_capabilities": None}) == []
    assert alp_project.som_unpopulated_capabilities(
        {"silicon_capabilities": "gpu2d"}) == []
    assert alp_project.som_unpopulated_capabilities(
        {"silicon_capabilities": {"unpopulated": None}}) == []
    assert alp_project.som_unpopulated_capabilities(
        {"silicon_capabilities": {"unpopulated": ["gpu2d", "dave2d"]}}
    ) == ["gpu2d", "dave2d"]


def test_v2n101_real_soc_drp_ai_true_and_cau_override(alp_project):
    """Integration test against the real RZ/V2N n44.json:
      - SoC-side drp_ai should be True.
      - SoM-side cau override (True) wins over silicon's cau: False.
      - SoM-only keys (optiga_trust_m, tmu_cordic) pass through.
    """
    preset = {
        "sku": "E1M-V2N101",
        "silicon": "renesas:rzv2n:n44",
        "capabilities": {
            "cau": True,
            "optiga_trust_m": True,
            "quadspi_dma": True,
            "tmu_cordic": True,
            "tmu_fft": True,
            "tmu_fac": True,
        },
    }
    result = alp_project.resolve_capabilities(preset, METADATA)

    # SoC-side facts
    assert result["drp_ai"] is True
    assert result["neon"] is True
    assert result["emmc_dma"] is True

    # SoM overrides silicon default
    assert result["cau"] is True          # silicon has cau: false; SoM bridge overrides
    assert result["quadspi_dma"] is True  # silicon has quadspi_dma: false; SoM bridge overrides

    # SoM-only extension keys
    assert result["optiga_trust_m"] is True
    assert result["tmu_cordic"] is True
    assert result["tmu_fft"] is True
    assert result["tmu_fac"] is True


def test_aen701_real_soc_silicon_caps_present(alp_project):
    """Integration test against the real Alif E7 JSON:
      - SoC-side helium_mve, cryptocell, xspi_dma are True.
      - SoM-only optiga_trust_m passes through.
      - cau stays False (E7 silicon has no CAU; no SoM override).
    """
    preset = {
        "sku": "E1M-AEN701",
        "silicon": "alif:ensemble:e7",
        "capabilities": {"optiga_trust_m": True},
    }
    result = alp_project.resolve_capabilities(preset, METADATA)

    assert result["helium_mve"] is True
    assert result["neon"] is True
    assert result["cryptocell"] is True
    assert result["xspi_dma"] is True
    assert result["cau"] is False          # no bridge on AEN701
    assert result["optiga_trust_m"] is True
