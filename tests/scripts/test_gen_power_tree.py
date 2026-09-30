# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/gen_power_tree.py -- the V2N-family PMIC guard tables.

- default_window(): target +/-5 % rounded INWARD to each chip's VSET grid
  (including the ACT88760 LDO1/2 12.5 mV grid, which lands between whole mV);
- cross_check() is clean on the real metadata and FAILS on each class of
  defect it exists to catch (widened window, window on a TBD rail, address
  not declared by a SoM preset, write: deny overlapping a rail register,
  missing GPIO) -- every mutation below was run against the real tree and
  must turn a clean result into a named error;
- the committed include/alp/chips/v2n_power_tree.h is byte-identical to a
  fresh render (skipped without the pinned clang-format, see conftest).
"""

from __future__ import annotations

import copy
from pathlib import Path

import pytest
import yaml

import gen_power_tree as g
from .conftest import clang_format_text

REPO = Path(__file__).resolve().parents[2]
TREE = REPO / "metadata" / "e1m_modules" / "v2n" / "power-tree.yaml"
HEADER = REPO / "include" / "alp" / "chips" / "v2n_power_tree.h"


@pytest.fixture(scope="module")
def real():
    tree = g.load_tree(TREE)
    chips = g.load_chips()
    presets = {p.stem: yaml.safe_load(p.read_text(encoding="utf-8"))
               for p in (REPO / "metadata" / "e1m_modules").glob("E1M-V2*.yaml")}
    return tree, chips, presets


def _rail(tree, rid):
    return next(r for r in tree["rails"] if r["id"] == rid)


@pytest.mark.parametrize("target,base_uv,step_uv,want", [
    (800, 500_000, 12_500, [763, 837]),     # ACT LDO1 range 0: 762.5 / 837.5 mV grid points
    (750, 300_000, 5_000, [715, 785]),      # DA9292 CH2 VSTEP=0
    (900, 500_000, 5_000, [855, 945]),      # ACT Buck7 range 0 -- exact on grid
    (3300, 500_000, 25_000, [3150, 3450]),  # ACT Buck1 range 1
    (850, 400_000, 5_000, [810, 890]),      # TPS628640 0x48
])
def test_default_window_rounds_inward(target, base_uv, step_uv, want):
    lo, hi = g.default_window(target, base_uv, step_uv, 5)
    assert [lo, hi] == want
    assert lo >= target * 0.95 and hi <= target * 1.05


def test_real_power_tree_is_clean(real):
    tree, chips, presets = real
    assert g.cross_check(tree, chips, presets, g.load_ownership(TREE)) == []


def test_cm33_bus_master_contradicting_core_ownership_fails(real):
    """The 2026-09-24 decision gives RIIC8 + P64/P65 to the A55: a boot
    mode naming cm33 as BRD_I2C master / DEEPX owner must fail."""
    tree, chips, presets = real
    tree = copy.deepcopy(tree)
    tree["boot_modes"]["cm33_boot"] = {"status": "supported", "bus_master": "cm33",
                                       "deepx_sequence_owner": "cm33"}
    errs = g.cross_check(tree, chips, presets, g.load_ownership(TREE))
    assert any("bus_master cm33" in e for e in errs)
    assert any("owns the DEEPX sequence" in e for e in errs)


def test_blocked_boot_mode_with_runtime_owner_fails(real):
    tree, chips, presets = real
    tree = copy.deepcopy(tree)
    _rail(tree, "vdd_0p75")["owner"]["cm33_boot"] = "cm33"
    assert any("cm33_boot: blocked" in e for e in g.cross_check(tree, chips, presets))


def test_every_v2n_preset_is_actually_consulted(real):
    tree, chips, presets = real
    # Guard against the preset-family mapping silently matching nothing:
    # drop ACT88760 from one preset and the check must notice.
    presets = copy.deepcopy(presets)
    devs = presets["E1M-V2N101"]["on_module"]["i2c_devices"]["brd_i2c"]["devices"]
    presets["E1M-V2N101"]["on_module"]["i2c_devices"]["brd_i2c"]["devices"] = [
        d for d in devs if d["chip"] != "act8760"]
    errs = g.cross_check(tree, chips, presets)
    assert any("E1M-V2N101 declares no act8760" in e for e in errs)


def test_widened_window_is_rejected(real):
    tree, chips, presets = real
    tree = copy.deepcopy(tree)
    _rail(tree, "vdd_0p75")["window_mv"] = [700, 800]
    assert any("vdd_0p75: window_mv" in e for e in g.cross_check(tree, chips, presets))


def test_voltage_control_on_tbd_rail_is_rejected(real):
    tree, chips, presets = real
    tree = copy.deepcopy(tree)
    _rail(tree, "da9292_ch1")["control"] = "voltage"
    assert any("da9292_ch1: control 'voltage' needs" in e for e in g.cross_check(tree, chips, presets))


def test_deny_overlapping_rail_register_is_rejected(real):
    tree, chips, presets = real
    chips = copy.deepcopy(chips)
    chips["act8760"]["register_table"].append(
        {"slave": "add1", "addr": 0x42, "name": "X", "type": "rw", "write": "deny", "summary": "x"})
    assert any("act8760 buck1" in e for e in g.cross_check(tree, chips, presets))


def test_tps_manifest_disagreement_is_rejected(real):
    tree, chips, presets = real
    tree = copy.deepcopy(tree)
    _rail(tree, "ddr5_vdd2h_1v05")["net"] = "DDR5_VDD"
    assert any("tps628640.yaml 0x44" in e for e in g.cross_check(tree, chips, presets))


def test_missing_gpio_is_rejected(real):
    tree, chips, presets = real
    tree = copy.deepcopy(tree)
    tree["gpios"] = [x for x in tree["gpios"] if x["gpio"] != 11]
    assert any("GPIO1..GPIO11" in e for e in g.cross_check(tree, chips, presets))


def test_rail_absent_from_family_gets_no_control(real):
    tree, _, _ = real
    text = g.render(tree, "v2n")
    base = text.split("V2N_POWER_DA9292_CH_LIMITS_INIT")[1].split("V2N_POWER_TPS628640")[0]
    assert ".voltage_writable = true" not in base  # CH2 is v2n-m1 only
    assert "#define V2N_POWER_TPS628640_VDD0V85_LPDDR_ADDR" not in text
    assert "#define V2N_M1_POWER_TPS628640_VDD0V85_LPDDR_ADDR 0x48u" in text
    assert "#define V2N_POWER_ACT8760_GPIO_POLARITY_WRITABLE_MASK 0x0008u" in text


def test_committed_header_in_sync(real, tmp_path):
    tree, _, _ = real
    fresh = clang_format_text(tmp_path, "v2n_power_tree.h", g.render(tree, "v2n"))
    assert HEADER.read_text(encoding="utf-8") == fresh, "run: python3 scripts/gen_power_tree.py"
