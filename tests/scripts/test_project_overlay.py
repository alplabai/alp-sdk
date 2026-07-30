# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_project.py -- dts-overlay emission
(TestDtsOverlayEmit) and native-sim-overlay emission
(TestNativeSimOverlayEmit).

scripts/alp_project.py is scheduled for deletion (the planner it fronted
relocated to the tan repo, issue #285); a subprocess test that spawns it
dies the day the file goes.  Where a committed `--emit` snapshot under
tests/fixtures/emit-snapshots/ (see scripts/check_emit_snapshots.py's
CASES for the board.yaml + mode each `<id>.snap` pins) genuinely contains
the text a test asserts on, that test now reads the snapshot file
directly instead of spawning alp_project.py.

THIS IS A DEMOTION, STATED HONESTLY: a snapshot-backed test proves the
COMMITTED ARTEFACT still says what the test expects -- it does NOT prove a
live `alp_project.py --emit ...` run on the test's original input would
still produce that text.  Most of the converted tests below never ran
against one of the three CASES boards in the first place -- they ran
against metadata/templates/board.yaml.example (TEMPLATE), and now read
the committed `proj-nsim` snapshot (examples/peripheral-io/spi-slave/
board.yaml) instead, because TEMPLATE and spi-slave share the same
E1M-AEN801 SoM / e1m-evk preset and neither declares an i2c or adc
peripheral -- so the generic bus-alias wiring and the full 52-slot
canonical pin array, the only things these tests check, are identical
between the two.  A change to alp_project.py that broke overlay
rendering only for something TEMPLATE exercises and spi-slave does not
would no longer be caught here.

WHAT WAS LOST, deliberately: every test whose board or scenario has no
CASES counterpart stays subprocess-based, unconverted:

  * test_overlay_aen_adc_uses_requested_e1m_adc_instance --
    examples/peripheral-io/adc-voltmeter/board.yaml is not one of the
    three CASES boards (aen-analog-validate / v2n-power-monitor /
    spi-slave); no snapshot carries its specific ADC-instance wiring.
  * test_overlay_aen_i2c1_is_not_suppressed_by_i2c0_catalog_wiring and
    the four i3c tests (HE slice / HP slice / unscoped-follows /
    unscoped-skips) -- each writes a SYNTHETIC board.yaml into tmp_path
    (a `pins:`/`cores:`/`peripherals:` shape no CASES board declares);
    no committed artefact represents that input.
  * test_e1m_x_overlay_uses_e1m_x_gpio_namespace and
    test_e1m_x_overlay_splits_99_pads_across_four_gpio_emul_controllers
    -- examples/display/lvgl-dashboard-x-evk/board.yaml (E1M-X family)
    is not a CASES board either.

These all still subprocess scripts/alp_project.py via _run_loader and
will need a real fix (a new corpus snapshot for their board, or a
rewrite against whatever replaces alp_project.py) once it is deleted --
this slice does not pretend otherwise.

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

SNAP_DIR = REPO / "tests" / "fixtures" / "emit-snapshots"


def _snapshot(snap_id: str) -> str:
    """Read a committed `--emit` snapshot (see check_emit_snapshots.py's
    CASES for the board.yaml + mode each `<snap_id>.snap` pins)."""
    return (SNAP_DIR / f"{snap_id}.snap").read_text(encoding="utf-8")


class TestDtsOverlayEmit(unittest.TestCase):
    """dts-overlay emission -- structural checks (well-formed
    blocks; bus + pin-array aliases present for E1M-EVK)."""

    def test_overlay_has_root_node_and_aliases(self) -> None:
        """Reads the committed proj-nsim snapshot (spi-slave board.yaml,
        E1M-AEN801/e1m-evk) -- see module docstring for why that stands
        in for TEMPLATE here."""
        out = _snapshot("proj-nsim.dts-overlay")
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
        I2C) ONLY when the peripheral is declared in board.yaml.  Reads
        the committed proj-nsim snapshot (spi-slave board.yaml,
        E1M-AEN801/e1m-evk, no i2c/adc declared) -- the same generic-alias
        facts TEMPLATE renders, since neither declares i2c or adc -- and
        the generic loop must NOT re-introduce the broken
        controller-pointing alp-i2cN / alp-adcN (see
        test_overlay_aen_family_does_not_emit_generic_i2c_adc_aliases)."""
        out = _snapshot("proj-nsim.dts-overlay")
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

        The committed proj-nsim snapshot (spi-slave board.yaml,
        E1M-AEN801/e1m-evk) declares only `spi`, no i2c/adc, so neither
        the generic loop nor the catalog emits an i2c/adc alias --
        proving the generic controller-pointing alias is gone.  Same
        proof TEMPLATE would give (it declares no peripherals at all)."""
        out = _snapshot("proj-nsim.dts-overlay")
        # No generic controller-pointing aliases for the catalog-owned
        # peripherals (these are exactly the duplicates the fix removes).
        self.assertNotIn("alp-i2c0 = &i2c", out)
        self.assertNotIn("alp-adc0 = &adc", out)
        # And since spi-slave declares no i2c/adc peripheral, the catalog
        # emits nothing either -- so the aliases are absent entirely.
        self.assertNotIn("alp-i2c", out)
        self.assertNotIn("alp-adc", out)

    def test_overlay_aen_adc_uses_requested_e1m_adc_instance(self) -> None:
        # examples/peripheral-io/adc-voltmeter/board.yaml is not a CASES
        # board (see module docstring) -- no committed snapshot to read.
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
        # Synthetic board.yaml, not a CASES board -- no snapshot exists.
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

    def test_overlay_aen_i3c_emits_lpi3c0_alias_for_he_slice(self) -> None:
        """AEN801 M55-HE slice declaring `peripherals: [i3c]` gets the
        lpi3c0 node enabled + pinctrl + the alp-i3c0 alias.  lpi3c0 is the
        M55-HE local-domain controller (IRQ 50); it shares pads P7_6/P7_7
        with the main i3c0 through a different pad mux, and firmware picks
        the owner.

        Synthetic board.yaml, not a CASES board -- no snapshot exists."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-AEN801
                preset: e1m-evk
                cores:
                  m55_he:
                    app: ./src
                    peripherals:
                      - i3c
            """)
            rv = _run_loader(input_path=path, emit="dts-overlay", core="m55_he")
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        out = rv.stdout
        self.assertIn("alp-i3c0 = &lpi3c0;", out)
        self.assertIn("&lpi3c0 {", out)
        self.assertIn('status = "okay";', out)
        self.assertIn("pinctrl_lpi3c0", out)

    def test_overlay_aen_i3c_emits_nothing_for_hp_slice(self) -> None:
        """An M55-HP slice never gets the lpi3c0 alias -- it's the M55-HE
        local domain (IRQ 50); alp_i3c_open() on HP must surface
        NOT_READY, never bind to a dead IRQ.

        Synthetic board.yaml, not a CASES board -- no snapshot exists."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-AEN801
                preset: e1m-evk
                cores:
                  m55_hp:
                    app: ./src
                    peripherals:
                      - i3c
            """)
            rv = _run_loader(input_path=path, emit="dts-overlay", core="m55_hp")
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        out = rv.stdout
        self.assertNotIn("alp-i3c0", out)
        self.assertNotIn("lpi3c0", out)

    def test_overlay_aen_i3c_unscoped_emit_follows_the_declared_cores(self) -> None:
        """The UNSCOPED emit (no --core) must not silently drop i3c wiring.

        Regression guard: gating on `v2_core_id != "m55_he"` alone skips the
        unscoped path, where v2_core_id is None -- so a board whose Zephyr
        core IS m55_he got no lpi3c0 alias while the conf emit still set
        CONFIG_I3C=y, i.e. a green build whose alp_i3c_open() returns
        NOT_READY with nothing to point at.

        Synthetic board.yaml, not a CASES board -- no snapshot exists.
        """
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-AEN801
                preset: e1m-evk
                cores:
                  m55_he:
                    app: ./src
                    peripherals:
                      - i3c
            """)
            rv = _run_loader(input_path=path, emit="dts-overlay")
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        self.assertIn("alp-i3c0 = &lpi3c0;", rv.stdout)

    def test_overlay_aen_i3c_unscoped_emit_skips_a_he_less_project(self) -> None:
        """...and the same unscoped path emits nothing once m55_he is OFF.

        Note m55_he must be turned off explicitly: the AEN801 topology
        supplies it with `effective_os: zephyr` even when board.yaml names
        only m55_hp, so an unscoped union legitimately covers it.

        Synthetic board.yaml, not a CASES board -- no snapshot exists.
        """
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-AEN801
                preset: e1m-evk
                cores:
                  m55_he:
                    os: "off"
                  m55_hp:
                    app: ./src
                    peripherals:
                      - i3c
            """)
            rv = _run_loader(input_path=path, emit="dts-overlay")
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        self.assertNotIn("lpi3c0", rv.stdout)

    def test_overlay_emits_alp_pin_array(self) -> None:
        out = _snapshot("proj-nsim.dts-overlay")
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
        out = _snapshot("proj-nsim.dts-overlay")
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
        out = _snapshot("proj-nsim.native-sim-overlay")
        self.assertIn("#include <zephyr/dt-bindings/gpio/gpio.h>", out)
        self.assertIn('compatible = "zephyr,gpio-emul"', out)
        self.assertIn('compatible = "alp,pin-array"', out)
        self.assertTrue(out.rstrip().endswith("};"),
                        msg="overlay must terminate with `};`")

    def test_two_gpio_emul_controllers_split_52_pads(self) -> None:
        """gpio-emul caps at 32 pins, so E1M's 52 pads span two
        controllers: gpio_emul0 (32) + gpio_emul1 (20)."""
        out = _snapshot("proj-nsim.native-sim-overlay")
        self.assertIn("gpio_emul0: gpio_emul0", out)
        self.assertIn("gpio_emul1: gpio_emul1", out)
        self.assertIn("ngpios = <32>", out)   # emul0 backs indices 0..31
        self.assertIn("ngpios = <20>", out)   # emul1 backs indices 32..51

    def test_pin_array_is_positional_52(self) -> None:
        """Full 52-entry positional map so alp_z_gpio_resolve(pin_id)
        resolves any pad under native_sim (ALP_PIN_COUNT = DT gpios len)."""
        out = _snapshot("proj-nsim.native-sim-overlay")
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
        # examples/display/lvgl-dashboard-x-evk/board.yaml is not a CASES
        # board (see module docstring) -- no committed snapshot to read.
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
