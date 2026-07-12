# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_project.py -- dts-overlay emission
(TestDtsOverlayEmit) and native-sim-overlay emission
(TestNativeSimOverlayEmit).

Run locally:

    python -m unittest tests.scripts.test_project_overlay

Or via CI as configured in .github/workflows/pr-metadata-validate.yml.
"""

from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _project_support import REPO, TEMPLATE, X_EVK_LVGL, _run_loader, _write_board  # noqa: E402


class TestDtsOverlayEmit(unittest.TestCase):
    """dts-overlay emission -- structural checks (well-formed
    blocks; bus + pin-array aliases present for E1M-EVK)."""

    def test_overlay_has_root_node_and_aliases(self) -> None:
        rv = _run_loader(input_path=TEMPLATE, emit="dts-overlay")
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        out = rv.stdout
        # Header + dt-bindings include
        self.assertIn('#include <zephyr/dt-bindings/gpio/gpio.h>', out)
        # Root node + the board comment line
        self.assertIn("/ {", out)
        self.assertIn("Board: E1M-EVK", out)
        # Aliases block
        self.assertIn("aliases {", out)
        # Closing brace + semicolon for the root node
        self.assertTrue(out.rstrip().endswith("};"),
                        msg="overlay must terminate with `};`")

    def test_overlay_emits_expected_bus_aliases_for_evk(self) -> None:
        """E1M-EVK wires SPI1, UART0+UART1, PWM0..PWM6, and DAC0..DAC1 as
        generic controller aliases derived from the board header (SoM
        mounting).  The ADC/DAC aliases feed the portable <alp/adc.h> /
        <alp/dac.h> backends, which resolve their channels via the
        alp-adcN / alp-dacN DT aliases.

        I2C and ADC are EXCLUDED here: for the `aen` SoM family they are
        owned by the _PERIPH_DT_WIRING catalog, which emits the correct
        alias (the io-channels consumer for ADC, the &i2c2 controller for
        I2C) ONLY when the peripheral is declared in board.yaml.  The
        TEMPLATE declares no peripherals, so no i2c/adc alias is emitted
        -- and the generic loop must NOT re-introduce the broken
        controller-pointing alp-i2cN / alp-adcN (see
        test_overlay_aen_family_does_not_emit_generic_i2c_adc_aliases)."""
        rv = _run_loader(input_path=TEMPLATE, emit="dts-overlay")
        out = rv.stdout
        for alias in ("alp-spi1",
                      "alp-uart0", "alp-uart1",
                      "alp-pwm0", "alp-pwm6",
                      "alp-dac0", "alp-dac1"):
            with self.subTest(alias=alias):
                self.assertIn(alias, out)

    def test_overlay_aen_family_does_not_emit_generic_i2c_adc_aliases(
        self,
    ) -> None:
        """For the `aen` SoM family the I2C/ADC buckets are catalog-owned:
        the generic _BUS_BUCKETS alias loop must skip them so the only
        alp-i2cN / alp-adcN aliases that ever appear are the correct ones
        the _PERIPH_DT_WIRING catalog emits for DECLARED peripherals.

        The TEMPLATE (E1M-AEN701, family `aen`) declares no peripherals,
        so neither the generic loop nor the catalog should emit an i2c/adc
        alias -- proving the generic controller-pointing alias is gone."""
        rv = _run_loader(input_path=TEMPLATE, emit="dts-overlay")
        out = rv.stdout
        # No generic controller-pointing aliases for the catalog-owned
        # peripherals (these are exactly the duplicates the fix removes).
        self.assertNotIn("alp-i2c0 = &i2c", out)
        self.assertNotIn("alp-adc0 = &adc", out)
        # And since the template declares no peripherals, the catalog
        # emits nothing either -- so the aliases are absent entirely.
        self.assertNotIn("alp-i2c", out)
        self.assertNotIn("alp-adc", out)

    def test_overlay_aen_adc_uses_requested_e1m_adc_instance(self) -> None:
        example = REPO / "examples" / "peripheral-io" / "adc-voltmeter" / "board.yaml"
        rv = _run_loader(input_path=example, emit="dts-overlay")
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        out = rv.stdout
        self.assertIn("alp_adc_in1: alp-adc-in1", out)
        self.assertIn("io-channels = <&adc12_0 1>;", out)
        self.assertIn("alp-adc1 = &alp_adc_in1;", out)
        self.assertIn("channel@1", out)
        self.assertNotIn("alp-adc0 = &alp_adc_in0;", out)
        self.assertNotIn("alp-adc1 = &adc1;", out)

    def test_overlay_aen_i2c1_is_not_suppressed_by_i2c0_catalog_wiring(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-AEN801
                preset: e1m-evk
                pins:
                  - E1M_I2C1
                cores:
                  m55_hp:
                    app: ./src
                    peripherals:
                      - i2c
            """)
            rv = _run_loader(input_path=path, emit="dts-overlay")
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        out = rv.stdout
        self.assertIn("alp-i2c1 = &i2c1;", out)
        self.assertNotIn("alp-i2c0 = &i2c2;", out)
        self.assertNotIn("pinctrl_i2c2", out)

    def test_overlay_emits_alp_pin_array(self) -> None:
        rv = _run_loader(input_path=TEMPLATE, emit="dts-overlay")
        out = rv.stdout
        self.assertIn('compatible = "alp,pin-array"', out)
        self.assertIn("alp_pins:", out)
        # The pin-array gpios list must end with a semicolon-terminated
        # last entry (not a comma).  Match any line whose first non-
        # comment token is `>;` -- i.e. a triplet's terminator.
        self.assertRegex(out, r">;\s*/\*")

    def test_overlay_pin_array_is_positional_52(self) -> None:
        """The alp,pin-array is the full 52-entry positional map in
        e1m_pinout.h canonical order, so alp_z_gpio_resolve(pin_id) is a
        direct index -- including the secondary-function pads opened as
        GPIO (PWM/ENC/ADC/DAC).  Without this, alp_gpio_open(ALP_E1M_GPIO_PWM3)
        et al. can't resolve."""
        rv = _run_loader(input_path=TEMPLATE, emit="dts-overlay")
        out = rv.stdout
        # Exactly 52 positional slots.
        self.assertEqual(out.count("<&gpio0 0 GPIO_ACTIVE_HIGH>"), 52)
        # Canonical slots present + correctly indexed in the comments.
        self.assertIn("[ 0] ALP_E1M_GPIO_IO0", out)
        self.assertIn("[ 4] ALP_E1M_GPIO_IO4", out)
        self.assertIn("[29] ALP_E1M_GPIO_PWM3", out)   # RGB-red pad as GPIO
        self.assertIn("[42] ALP_E1M_GPIO_ADC0", out)
        self.assertIn("[51] ALP_E1M_GPIO_DAC1", out)


class TestNativeSimOverlayEmit(unittest.TestCase):
    """native-sim-overlay emission -- the canonical alp,pin-array on
    zephyr,gpio-emul so GPIO apps link + resolve under native_sim."""

    def test_overlay_structure(self) -> None:
        rv = _run_loader(input_path=TEMPLATE, emit="native-sim-overlay")
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        out = rv.stdout
        self.assertIn("#include <zephyr/dt-bindings/gpio/gpio.h>", out)
        self.assertIn('compatible = "zephyr,gpio-emul"', out)
        self.assertIn('compatible = "alp,pin-array"', out)
        self.assertTrue(out.rstrip().endswith("};"),
                        msg="overlay must terminate with `};`")

    def test_two_gpio_emul_controllers_split_52_pads(self) -> None:
        """gpio-emul caps at 32 pins, so E1M's 52 pads span two
        controllers: gpio_emul0 (32) + gpio_emul1 (20)."""
        out = _run_loader(input_path=TEMPLATE, emit="native-sim-overlay").stdout
        self.assertIn("gpio_emul0: gpio_emul0", out)
        self.assertIn("gpio_emul1: gpio_emul1", out)
        self.assertIn("ngpios = <32>", out)   # emul0 backs indices 0..31
        self.assertIn("ngpios = <20>", out)   # emul1 backs indices 32..51

    def test_pin_array_is_positional_52(self) -> None:
        """Full 52-entry positional map so alp_z_gpio_resolve(pin_id)
        resolves any pad under native_sim (ALP_PIN_COUNT = DT gpios len)."""
        out = _run_loader(input_path=TEMPLATE, emit="native-sim-overlay").stdout
        # 52 positional triplets total, split across the two controllers.
        self.assertEqual(out.count("GPIO_ACTIVE_HIGH>"), 52)
        self.assertIn("<&gpio_emul0  0 GPIO_ACTIVE_HIGH>", out)   # [ 0] IO0
        self.assertIn("<&gpio_emul0 31 GPIO_ACTIVE_HIGH>", out)   # [31] PWM5
        self.assertIn("<&gpio_emul1  0 GPIO_ACTIVE_HIGH>", out)   # [32] PWM6
        self.assertIn("<&gpio_emul1 19 GPIO_ACTIVE_HIGH>", out)   # [51] DAC1
        # Canonical slots present + correctly indexed in the comments.
        self.assertIn("[ 0] ALP_E1M_GPIO_IO0", out)
        self.assertIn("[31] ALP_E1M_GPIO_PWM5", out)
        self.assertIn("[32] ALP_E1M_GPIO_PWM6", out)
        self.assertIn("[42] ALP_E1M_GPIO_ADC0", out)
        self.assertIn("[51] ALP_E1M_GPIO_DAC1", out)
        # The last entry is semicolon-terminated (not a comma).
        self.assertRegex(out, r">;\s*/\*")

    def test_e1m_x_overlay_uses_e1m_x_gpio_namespace(self) -> None:
        rv = _run_loader(input_path=X_EVK_LVGL, emit="native-sim-overlay")
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        out = rv.stdout
        self.assertIn("E1M-X canonical order (e1m_x_pinout.h)", out)
        self.assertEqual(out.count("GPIO_ACTIVE_HIGH>"), 99)
        self.assertIn("[ 0] ALP_E1M_X_GPIO_IO0", out)
        self.assertIn("[62] ALP_E1M_X_GPIO_I2C2_SDA", out)
        self.assertIn("[73] ALP_E1M_X_GPIO_LCD_B0", out)
        self.assertIn("[98] ALP_E1M_X_GPIO_LCD_VSYNC", out)
        self.assertNotIn("ALP_E1M_GPIO_IO0", out)

    def test_e1m_x_overlay_splits_99_pads_across_four_gpio_emul_controllers(
        self,
    ) -> None:
        out = _run_loader(input_path=X_EVK_LVGL, emit="native-sim-overlay").stdout
        for idx in range(4):
            self.assertIn(f"gpio_emul{idx}: gpio_emul{idx}", out)
        self.assertEqual(out.count("ngpios = <32>"), 3)
        self.assertIn("ngpios = <3>", out)
        self.assertIn("<&gpio_emul2 31 GPIO_ACTIVE_HIGH>", out)  # [95] LCD_B22
        self.assertIn("<&gpio_emul3  2 GPIO_ACTIVE_HIGH>", out)  # [98] LCD_VSYNC


if __name__ == "__main__":
    unittest.main()
