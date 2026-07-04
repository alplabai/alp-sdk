# SPDX-License-Identifier: Apache-2.0
"""Tests for `--emit composed-route-table` in alp_project.py.

Runs the emitter via subprocess for one (board x SoM) pair and
validates the JSON shape.  Uses E1M-AEN701 + E1M-EVK because that
combination exercises both the board e1m_routes: block and the
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
# board.yaml used by the gpio-button-led example.
AEN701_EVK_BOARD = REPO / "examples" / "peripheral-io" / "gpio-button-led" / "board.yaml"

# E1M-V2N101 + E1M-X-EVK: the X-EVK now carries real e1m_routes, so the
# composition is a mix -- GD32 IO-MCU pads (PWM/ENC/ADC/DAC + the GPIOs
# V2N101.pad_routes lists) dispatch gd32_bridge, while pads V2N101 does
# not declare (the buses + not-yet-bridged GPIO) currently fall through
# to direct.  That fallthrough is TBD, not ground truth -- see the
# TestV2n101XEvk docstring.
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
    board_preset = alp_project._resolve_inline_or_preset_board(
        project, METADATA)
    raw = alp_project._emit_composed_route_table(
        project, sku_preset, board_preset, METADATA
    )
    return json.loads(raw)


class TestAen701Evk:
    """E1M-AEN701 + E1M-EVK: board has e1m_routes, SoM has CC3501E dispatch."""

    @pytest.fixture(scope="class")
    def result(self, alp_project):
        return _run_emitter(AEN701_EVK_BOARD, alp_project)

    def test_top_level_keys_present(self, result):
        for key in ("board", "som", "silicon_variant", "routes"):
            assert key in result, f"Top-level key '{key}' missing from output"

    def test_board_and_som_values(self, result):
        assert result["board"] == "E1M-EVK"
        assert result["som"] == "E1M-AEN701"

    def test_silicon_variant_resolved(self, result):
        # AEN701 carries AE722F80F55D5LS (Alif Ensemble E7).
        assert result["silicon_variant"] == "AE722F80F55D5LS"

    def test_routes_is_non_empty_list(self, result):
        routes = result["routes"]
        assert isinstance(routes, list)
        assert len(routes) > 0

    def test_each_route_has_required_fields(self, result):
        required = {"e1m", "board_category", "board_macro", "dispatch"}
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
        assert row["board_macro"] == "EVK_PIN_BMI323_INT1"
        assert row["dispatch"] == "cc3501e"
        assert row.get("dispatch_pin") == 14
        assert row.get("board_category") == "gpio"

    def test_direct_dispatch_pads_present(self, result):
        """Pads not in the AEN701's pad_routes default to dispatch: direct."""
        direct_rows = [r for r in result["routes"] if r.get("dispatch") == "direct"]
        assert len(direct_rows) > 0

    def test_active_low_flag_on_encoder_sw(self, result):
        """EVK_PIN_ENCODER_SW (E1M_GPIO_IO4) is active-low; the flag must
        propagate from the board YAML into the JSON row."""
        enc_rows = [r for r in result["routes"] if r.get("e1m") == "E1M_GPIO_IO4"]
        assert enc_rows, "E1M_GPIO_IO4 not found in route table"
        assert enc_rows[0].get("active_low") is True

    def test_pwm_routes_in_board_category_pwm(self, result):
        """EVK_PWM_LED_GREEN (E1M_PWM0) must appear under board_category pwm."""
        pwm_rows = [r for r in result["routes"] if r.get("board_category") == "pwm"]
        assert len(pwm_rows) > 0

    def test_bus_routes_in_board_category_buses(self, result):
        evk_i2c = [r for r in result["routes"]
                   if r.get("board_macro") == "EVK_I2C_BUS_SENSORS"]
        assert evk_i2c, "EVK_I2C_BUS_SENSORS not found in routes"
        assert evk_i2c[0]["board_category"] == "buses"

    def test_json_output_is_valid_json(self, alp_project):
        """The emitter must return well-formed JSON (no trailing garbage)."""
        project = alp_project._load_yaml(AEN701_EVK_BOARD)
        sku_preset = alp_project._resolve_sku(project["som"]["sku"], METADATA)
        board_preset = alp_project._resolve_board("e1m-evk", METADATA)
        raw = alp_project._emit_composed_route_table(
            project, sku_preset, board_preset, METADATA
        )
        parsed = json.loads(raw)
        assert isinstance(parsed, dict)


class TestV2n101XEvk:
    """E1M-V2N101 + E1M-X-EVK: the X-EVK carries real e1m_routes now, so
    the composition is a MIX (no longer the empty-skeleton all-SoM case):

      - GD32 IO-MCU pads -- PWM/ENC/ADC/DAC + the GPIOs V2N101.pad_routes
        lists -- dispatch `gd32_bridge`.
      - Pads V2N101.pad_routes does NOT declare -- the X-EVK buses
        (I2C/SPI/UART/I2S/CAN) plus GPIO IO0-7/15/17-23 and the
        LED-as-GPIO secondaries (E1M_X_GPIO_PWM5..7) -- currently fall
        through to `direct`.

    That `direct` for the undeclared pads is a fallthrough pending
    authoritative V2N routing, NOT ground truth: the real bus routing is
    mixed -- not every bus reaches the V2N SoC; some terminate on the GD32
    IO MCU / board-management bus -- and the undeclared GPIO is IO-MCU
    driven on hardware.  Completing V2N101.pad_routes resolves these, at
    which point the dispatch assertions below update deliberately.  Routes
    use the E1M_X_* namespace (e1m_x_pinout.h)."""

    @pytest.fixture(scope="class")
    def result(self, alp_project):
        return _run_emitter(V2N101_XEVK_BOARD, alp_project)

    def test_top_level_keys_present(self, result):
        for key in ("board", "som", "silicon_variant", "routes"):
            assert key in result

    def test_som_and_board_values(self, result):
        assert result["som"] == "E1M-V2N101"
        assert result["board"] == "E1M-X-EVK"

    def test_silicon_variant_resolved(self, result):
        assert result["silicon_variant"] == "R9A09G056N44GBG"

    def test_pwm0_dispatch_pin_is_pa11(self, result):
        """E1M_X_PWM0 -> GD32 PA11 (V2N101 pad_routes, E1M_X_* namespace)."""
        pwm0 = [r for r in result["routes"] if r.get("e1m") == "E1M_X_PWM0"]
        assert pwm0, "E1M_X_PWM0 not in route table"
        assert pwm0[0]["dispatch"] == "gd32_bridge"
        assert pwm0[0].get("dispatch_pin") == "PA11"

    def test_som_routed_pads_dispatch_gd32_bridge(self, result):
        """Pads V2N101.pad_routes declares route through the GD32 IO MCU,
        whether or not the board also names them."""
        by_e1m = {r["e1m"]: r for r in result["routes"]}
        for pad in ("E1M_X_PWM0", "E1M_X_ENC0", "E1M_X_ADC0", "E1M_X_ADC7",
                    "E1M_X_DAC0", "E1M_X_GPIO_IO9"):
            assert by_e1m[pad]["dispatch"] == "gd32_bridge", (
                f"{pad} expected gd32_bridge, got {by_e1m[pad]['dispatch']!r}"
            )

    def test_board_routed_rows_carry_board_fields(self, result):
        """A pad the X-EVK names surfaces its board macro + category."""
        by_e1m = {r["e1m"]: r for r in result["routes"]}
        assert by_e1m["E1M_X_I2C0"]["board_macro"] == "XEVK_I2C_BUS_SENSORS"
        assert by_e1m["E1M_X_I2C0"]["board_category"] == "buses"
        assert by_e1m["E1M_X_GPIO_PWM5"]["board_macro"] == "XEVK_PIN_LED_RED"
        assert by_e1m["E1M_X_GPIO_PWM5"]["board_category"] == "gpio"

    def test_som_only_pads_have_null_board_fields(self, result):
        """Pads V2N101 routes but the board does not name (e.g. the unused
        ADC channels) stay SoM-only: gd32_bridge + null board fields."""
        adc7 = {r["e1m"]: r for r in result["routes"]}["E1M_X_ADC7"]
        assert adc7["dispatch"] == "gd32_bridge"
        assert adc7.get("board_macro") is None
        assert adc7.get("board_category") is None

    def test_undeclared_pads_currently_default_direct(self, result):
        """KNOWN TODO -- documents the present emitter output, NOT an
        authoritative routing claim: pads V2N101.pad_routes does not
        declare currently fall through to `direct`.  The real routing is
        mixed -- some X-EVK buses reach the V2N SoC, others terminate on
        the GD32 IO MCU / board-management bus; the undeclared GPIO +
        LED-as-GPIO pads are IO-MCU driven on hardware.  Completing
        V2N101.pad_routes will flip the affected rows and require updating
        this test -- making the change visible rather than silent."""
        by_e1m = {r["e1m"]: r for r in result["routes"]}
        for pad in ("E1M_X_I2C0", "E1M_X_SPI1", "E1M_X_UART0", "E1M_X_CAN0",
                    "E1M_X_I2S0", "E1M_X_GPIO_IO0", "E1M_X_GPIO_PWM5"):
            assert by_e1m[pad]["dispatch"] == "direct"

    def test_route_table_has_both_dispatch_kinds(self, result):
        """The composition mixes gd32_bridge (V2N101.pad_routes) and direct
        (undeclared) rows.  Exact split today: 40 gd32_bridge + 25 direct =
        65; the direct count shrinks as V2N101.pad_routes gains the
        currently-undeclared pads (see the class docstring)."""
        routes = result["routes"]
        gd32 = [r for r in routes if r["dispatch"] == "gd32_bridge"]
        direct = [r for r in routes if r["dispatch"] == "direct"]
        assert len(gd32) == 40
        assert len(direct) == 25
        assert len(routes) == 65


class TestPerRevPadRouteOverride:
    """The composed route table differs by hw_rev: the base pad_routes track
    the production rev (r2); r1's pad_route_overrides restore the pre-2626-R2
    routing.  This is what makes `--emit composed-route-table` rev-differentiated
    (the byte-for-byte two-rev seam)."""

    @staticmethod
    def _dispatch(alp_project, hw_rev):
        sku_preset = alp_project._resolve_sku("E1M-AEN801", METADATA)
        project = {"name": "t", "som": {"sku": "E1M-AEN801", "hw_rev": hw_rev}}
        out = json.loads(alp_project._emit_composed_route_table(
            project, sku_preset, None, METADATA))
        assert out["hw_rev"] == hw_rev
        return {r["e1m"]: (r["dispatch"], r.get("dispatch_pin"))
                for r in out["routes"]
                if r["e1m"] in ("E1M_GPIO_IO8", "E1M_GPIO_IO10", "E1M_GPIO_IO21")}

    def test_r2_is_the_production_base(self, alp_project):
        d = self._dispatch(alp_project, "r2")
        assert d["E1M_GPIO_IO8"] == ("cc3501e", 30)
        assert d["E1M_GPIO_IO10"] == ("cc3501e", 35)
        assert "E1M_GPIO_IO21" not in d          # unrouted in r2

    def test_r1_override_restores_pre_r2_routing(self, alp_project):
        d = self._dispatch(alp_project, "r1")
        assert d["E1M_GPIO_IO8"] == ("direct", None)     # Alif GPIO
        assert d["E1M_GPIO_IO10"] == ("direct", None)    # Alif GPIO
        assert d["E1M_GPIO_IO21"] == ("cc3501e", 30)     # CC3501E GPIO_30

    def test_r1_and_r2_emit_differ(self, alp_project):
        assert self._dispatch(alp_project, "r1") != self._dispatch(alp_project, "r2")
