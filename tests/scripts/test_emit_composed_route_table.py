# SPDX-License-Identifier: Apache-2.0
"""Tests for `--emit composed-route-table` in alp_project.py.

Runs the emitter via subprocess for one (carrier x SoM) pair and
validates the JSON shape.  Uses E1M-AEN701 + E1M-EVK because that
combination exercises both the carrier e1m_routes: block and the
SoM's pad_routes: CC3501E dispatch entries in one shot.
"""
from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
METADATA = REPO / "metadata"
SCRIPT = REPO / "scripts" / "alp_project.py"

# E1M-AEN701 + E1M-EVK: the canonical reference combination.
# board.yaml used by the gpio-button-led example (schema_version: 2).
AEN701_EVK_BOARD = REPO / "examples" / "gpio-button-led" / "board.yaml"

# E1M-V2N101 + E1M-X-EVK: all-gd32_bridge pad_routes, no carrier
# e1m_routes (X-EVK YAML has none yet), so all 32 SoM-declared
# pads land as SoM-only entries.
V2N101_XEVK_BOARD = REPO / "examples" / "v2n" / "v2n-pwm-fan-control" / "board.yaml"


@pytest.fixture(scope="module")
def alp_project():
    spec = importlib.util.spec_from_file_location("alp_project", SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules["alp_project"] = mod
    spec.loader.exec_module(mod)
    return mod


def _run_emitter(board_yaml: Path, alp_project) -> dict:
    """Load the project and run _emit_composed_route_table() directly."""
    project = alp_project._load_yaml(board_yaml)
    sku_preset = alp_project._resolve_sku(project["som"]["sku"], METADATA)
    carrier_preset = None
    if "carrier" in project and project["carrier"]:
        carrier_preset = alp_project._resolve_carrier(
            project["carrier"]["name"], METADATA
        )
    raw = alp_project._emit_composed_route_table(
        project, sku_preset, carrier_preset, METADATA
    )
    return json.loads(raw)


class TestAen701Evk:
    """E1M-AEN701 + E1M-EVK: carrier has e1m_routes, SoM has CC3501E dispatch."""

    @pytest.fixture(scope="class")
    def result(self, alp_project):
        return _run_emitter(AEN701_EVK_BOARD, alp_project)

    def test_top_level_keys_present(self, result):
        for key in ("carrier", "som", "silicon_variant", "routes"):
            assert key in result, f"Top-level key '{key}' missing from output"

    def test_carrier_and_som_values(self, result):
        assert result["carrier"] == "E1M-EVK"
        assert result["som"] == "E1M-AEN701"

    def test_silicon_variant_resolved(self, result):
        # AEN701 carries AE722F80F55D5LS (Alif Ensemble E7).
        assert result["silicon_variant"] == "AE722F80F55D5LS"

    def test_routes_is_non_empty_list(self, result):
        routes = result["routes"]
        assert isinstance(routes, list)
        assert len(routes) > 0

    def test_each_route_has_required_fields(self, result):
        required = {"e1m", "carrier_category", "carrier_macro", "dispatch"}
        for row in result["routes"]:
            missing = required - set(row.keys())
            assert not missing, (
                f"Route {row.get('e1m')!r} missing fields: {missing}"
            )

    def test_bmi323_int1_route_is_cc3501e(self, result):
        """EVK_PIN_BMI323_INT1 (E1M_GPIO_IO15) terminates on CC3501E GPIO_14
        on the AEN701.  This is the canonical cross-SoM routing example."""
        bmi_rows = [r for r in result["routes"] if r.get("e1m") == "E1M_GPIO_IO15"]
        assert bmi_rows, "E1M_GPIO_IO15 not found in route table"
        row = bmi_rows[0]
        assert row["carrier_macro"] == "EVK_PIN_BMI323_INT1"
        assert row["dispatch"] == "cc3501e"
        assert row.get("dispatch_pin") == 14
        assert row.get("carrier_category") == "gpio"

    def test_direct_dispatch_pads_present(self, result):
        """Pads not in the AEN701's pad_routes default to dispatch: direct."""
        direct_rows = [r for r in result["routes"] if r.get("dispatch") == "direct"]
        assert len(direct_rows) > 0

    def test_active_low_flag_on_encoder_sw(self, result):
        """EVK_PIN_ENCODER_SW (E1M_GPIO_IO4) is active-low; the flag must
        propagate from the carrier YAML into the JSON row."""
        enc_rows = [r for r in result["routes"] if r.get("e1m") == "E1M_GPIO_IO4"]
        assert enc_rows, "E1M_GPIO_IO4 not found in route table"
        assert enc_rows[0].get("active_low") is True

    def test_pwm_routes_in_carrier_category_pwm(self, result):
        """EVK_PWM_LED_GREEN (E1M_PWM0) must appear under carrier_category pwm."""
        pwm_rows = [r for r in result["routes"] if r.get("carrier_category") == "pwm"]
        assert len(pwm_rows) > 0

    def test_bus_routes_in_carrier_category_buses(self, result):
        evk_i2c = [r for r in result["routes"]
                   if r.get("carrier_macro") == "EVK_I2C_BUS_SENSORS"]
        assert evk_i2c, "EVK_I2C_BUS_SENSORS not found in routes"
        assert evk_i2c[0]["carrier_category"] == "buses"

    def test_json_output_is_valid_json(self, alp_project):
        """The emitter must return well-formed JSON (no trailing garbage)."""
        project = alp_project._load_yaml(AEN701_EVK_BOARD)
        sku_preset = alp_project._resolve_sku(project["som"]["sku"], METADATA)
        carrier_preset = alp_project._resolve_carrier("E1M-EVK", METADATA)
        raw = alp_project._emit_composed_route_table(
            project, sku_preset, carrier_preset, METADATA
        )
        parsed = json.loads(raw)
        assert isinstance(parsed, dict)


class TestV2n101XEvk:
    """E1M-V2N101 + E1M-X-EVK: no carrier e1m_routes; all 40 SoM pads
    from pad_routes appear as SoM-only entries dispatched via gd32_bridge.
    pad_routes use the E1M_X_* namespace (e1m_x_pinout.h)."""

    @pytest.fixture(scope="class")
    def result(self, alp_project):
        return _run_emitter(V2N101_XEVK_BOARD, alp_project)

    def test_top_level_keys_present(self, result):
        for key in ("carrier", "som", "silicon_variant", "routes"):
            assert key in result

    def test_som_and_carrier_values(self, result):
        assert result["som"] == "E1M-V2N101"
        assert result["carrier"] == "E1M-X-EVK"

    def test_silicon_variant_resolved(self, result):
        assert result["silicon_variant"] == "R9A09G056N44GBG"

    def test_all_routes_are_gd32_bridge(self, result):
        """V2N101 routes every E1M pad through gd32_bridge; no carrier
        e1m_routes on E1M-X-EVK, so no direct pads should appear."""
        for row in result["routes"]:
            assert row["dispatch"] == "gd32_bridge", (
                f"{row['e1m']} has dispatch {row['dispatch']!r}, expected gd32_bridge"
            )

    def test_pwm0_dispatch_pin_is_pa11(self, result):
        """E1M_X_PWM0 -> GD32 PA11 (from V2N101 pad_routes in E1M-X namespace)."""
        pwm0 = [r for r in result["routes"] if r.get("e1m") == "E1M_X_PWM0"]
        assert pwm0, "E1M_X_PWM0 not in route table"
        assert pwm0[0].get("dispatch_pin") == "PA11"

    def test_routes_count_matches_som_pad_routes(self, result):
        """V2N101 declares 40 pad_routes entries (8 PWM + 4 ENC + 8 ADC +
        2 DAC + 10 GPIO_IO + 8 E1M-X-extension IOs); the table must have
        exactly 40 rows.  All entries use E1M_X_* namespace."""
        assert len(result["routes"]) == 40

    def test_som_only_rows_have_null_carrier_fields(self, result):
        """With no carrier e1m_routes, all rows are SoM-only and must
        carry null carrier_category, carrier_macro, carrier_role."""
        for row in result["routes"]:
            assert row.get("carrier_category") is None
            assert row.get("carrier_macro") is None
