# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_project.py -- the v0.3 board.yaml loader.

Run locally:

    python -m unittest tests.scripts.test_alp_project

Or via CI as configured in .github/workflows/pr-metadata-validate.yml.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import textwrap
import unittest
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
LOADER = REPO / "scripts" / "alp_project.py"
TEMPLATE = REPO / "metadata" / "templates" / "board.yaml.example"


def _run_loader(
    *,
    input_path: Path,
    emit: str = "zephyr-conf",
) -> subprocess.CompletedProcess[str]:
    """Invoke alp_project.py and return the completed process.  Uses
    the same interpreter the test suite is running under so the
    pyyaml + jsonschema deps line up."""
    return subprocess.run(
        [sys.executable, str(LOADER),
         "--input", str(input_path),
         "--emit", emit],
        capture_output=True, text=True, check=False,
    )


def _write_board(tmp: Path, body: str) -> Path:
    path = tmp / "board.yaml"
    path.write_text(textwrap.dedent(body), encoding="utf-8")
    return path


class TestLoaderContract(unittest.TestCase):
    """Schema + preset resolution behaviour."""

    def test_canonical_template_passes(self) -> None:
        """The shipped board.yaml.example must compile cleanly in
        every emit mode -- it's the canonical reference customers
        copy from."""
        for emit in ("zephyr-conf", "cmake-args", "yocto-conf", "dts-overlay"):
            with self.subTest(emit=emit):
                rv = _run_loader(input_path=TEMPLATE, emit=emit)
                self.assertEqual(rv.returncode, 0,
                                 msg=f"{emit}: stderr={rv.stderr}")
                self.assertTrue(rv.stdout.strip(),
                                msg=f"{emit}: empty output")

    def test_minimum_viable_board_yaml(self) -> None:
        """A board.yaml with just schema_version + som + os should be
        accepted (carrier is optional in the schema)."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                schema_version: 1
                som:
                  sku: E1M-AEN701
                os: zephyr
            """)
            rv = _run_loader(input_path=path)
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            self.assertIn("CONFIG_ALP_SDK=y", rv.stdout)
            self.assertIn("CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y", rv.stdout)

    def test_bad_sku_pattern_fails_schema(self) -> None:
        """The schema enforces the E1M-* SKU pattern; an arbitrary
        string must fail validation with a non-zero exit."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                schema_version: 1
                som:
                  sku: NOT-A-REAL-SKU
                os: zephyr
            """)
            rv = _run_loader(input_path=path)
            self.assertNotEqual(rv.returncode, 0)
            self.assertIn("schema violation", rv.stderr.lower())

    def test_unknown_sku_with_valid_pattern_fails_preset_lookup(self) -> None:
        """A SKU that matches the pattern but has no preset on disk
        must produce a clear missing-preset error, not a schema error."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                schema_version: 1
                som:
                  sku: E1M-NX9999
                os: zephyr
            """)
            rv = _run_loader(input_path=path)
            self.assertNotEqual(rv.returncode, 0)
            self.assertIn("no preset", rv.stderr.lower())

    def test_carrier_override_flips_populated_flag(self) -> None:
        """A user override under carrier.populated must win over the
        EVK preset's default -- bme280 ships false on the EVK; the
        override flips it to true."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                schema_version: 1
                som:
                  sku: E1M-AEN701
                carrier:
                  name: E1M-EVK
                  populated:
                    bme280: true
                os: zephyr
            """)
            rv = _run_loader(input_path=path)
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            self.assertIn("CONFIG_ALP_SDK_CHIP_BME280=y", rv.stdout)


class TestZephyrEmit(unittest.TestCase):
    """zephyr-conf emission specifics -- the baseline + log-level
    mapping introduced alongside the v0.3 single-rsource workflow."""

    def test_baseline_alp_sdk_always_on(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                schema_version: 1
                som:
                  sku: E1M-AEN701
                os: zephyr
            """)
            rv = _run_loader(input_path=path)
            self.assertIn("CONFIG_ALP_SDK=y", rv.stdout)
            self.assertIn("CONFIG_LOG=y", rv.stdout)
            self.assertIn("CONFIG_PRINTK=y", rv.stdout)

    def test_peripherals_emit_subsystem_kconfig(self) -> None:
        """Each entry under `peripherals:` must produce the matching
        Zephyr CONFIG_<SUBSYS>=y line.  Covers the per-peripheral
        example migrations (adc-voltmeter, i2c-scanner, ...) so a
        regression in the mapping table is caught at the metadata
        gate, not at twister time."""
        cases = {
            "adc":      "CONFIG_ADC=y",
            "can":      "CONFIG_CAN=y",
            "counter":  "CONFIG_COUNTER=y",
            "gpio":     "CONFIG_GPIO=y",
            "i2c":      "CONFIG_I2C=y",
            "i2s":      "CONFIG_I2S=y",
            "pwm":      "CONFIG_PWM=y",
            "rtc":      "CONFIG_RTC=y",
            "sensor":   "CONFIG_SENSOR=y",
            "spi":      "CONFIG_SPI=y",
            "uart":     "CONFIG_SERIAL=y",
            "watchdog": "CONFIG_WATCHDOG=y",
        }
        for periph, kconfig in cases.items():
            with self.subTest(peripheral=periph):
                with tempfile.TemporaryDirectory() as td:
                    path = _write_board(Path(td), f"""
                        schema_version: 1
                        som:
                          sku: E1M-AEN701
                        os: zephyr
                        peripherals:
                          - {periph}
                    """)
                    rv = _run_loader(input_path=path)
                    self.assertEqual(rv.returncode, 0, msg=rv.stderr)
                    self.assertIn(kconfig, rv.stdout)

    def test_log_level_maps_to_kconfig_default(self) -> None:
        cases = {"error": 1, "warn": 2, "info": 3, "debug": 4, "trace": 4}
        for log_level, kconfig_n in cases.items():
            with self.subTest(log_level=log_level):
                with tempfile.TemporaryDirectory() as td:
                    path = _write_board(Path(td), f"""
                        schema_version: 1
                        som:
                          sku: E1M-AEN701
                        os: zephyr
                        diagnostics:
                          log_level: {log_level}
                    """)
                    rv = _run_loader(input_path=path)
                    self.assertEqual(rv.returncode, 0)
                    self.assertIn(f"CONFIG_LOG_DEFAULT_LEVEL={kconfig_n}",
                                  rv.stdout)


class TestDtsOverlayEmit(unittest.TestCase):
    """dts-overlay emission -- structural checks (well-formed
    blocks; bus + pin-array aliases present for E1M-EVK)."""

    def test_overlay_has_root_node_and_aliases(self) -> None:
        rv = _run_loader(input_path=TEMPLATE, emit="dts-overlay")
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        out = rv.stdout
        # Header + dt-bindings include
        self.assertIn('#include <zephyr/dt-bindings/gpio/gpio.h>', out)
        # Root node + the carrier comment line
        self.assertIn("/ {", out)
        self.assertIn("Carrier: E1M-EVK", out)
        # Aliases block
        self.assertIn("aliases {", out)
        # Closing brace + semicolon for the root node
        self.assertTrue(out.rstrip().endswith("};"),
                        msg="overlay must terminate with `};`")

    def test_overlay_emits_expected_bus_aliases_for_evk(self) -> None:
        """E1M-EVK wires I2C0+I2C1, SPI1, UART0+UART1, and PWM0..PWM6."""
        rv = _run_loader(input_path=TEMPLATE, emit="dts-overlay")
        out = rv.stdout
        for alias in ("alp-i2c0", "alp-i2c1",
                      "alp-spi1",
                      "alp-uart0", "alp-uart1",
                      "alp-pwm0", "alp-pwm6"):
            with self.subTest(alias=alias):
                self.assertIn(alias, out)

    def test_overlay_emits_alp_pin_array(self) -> None:
        rv = _run_loader(input_path=TEMPLATE, emit="dts-overlay")
        out = rv.stdout
        self.assertIn('compatible = "alp,pin-array"', out)
        self.assertIn("alp_pins:", out)
        # The pin-array gpios list must end with a semicolon-terminated
        # last entry (not a comma).  Match any line whose first non-
        # comment token is `>;` -- i.e. a triplet's terminator.
        self.assertRegex(out, r">;\s*/\*")


class TestHwInfoHEmit(unittest.TestCase):
    """hw-info-h emission -- companion to the public <alp/hw_info.h>.
    Bakes board.yaml identifiers into ALP_HW_BUILD_* macros so apps
    can pass them to alp_hw_info_assert_matches_build() at runtime."""

    def test_canonical_template_emits_expected_macros(self) -> None:
        rv = _run_loader(input_path=TEMPLATE, emit="hw-info-h")
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        out = rv.stdout
        # Header guards + the always-emitted macros for the
        # canonical template (which sets every field).
        self.assertIn("#ifndef ALP_HW_INFO_BUILD_H", out)
        self.assertIn("#define ALP_HW_INFO_BUILD_H", out)
        self.assertIn('#define ALP_HW_BUILD_SOM_SKU         "E1M-AEN701"', out)
        self.assertIn('#define ALP_HW_BUILD_SOM_FAMILY      "aen"', out)
        self.assertIn('#define ALP_HW_BUILD_SOM_HW_REV      "r1"', out)
        self.assertIn('#define ALP_HW_BUILD_CARRIER_NAME    "E1M-EVK"', out)
        self.assertIn('#define ALP_HW_BUILD_CARRIER_HW_REV  "r1"', out)
        self.assertIn('#define ALP_HW_BUILD_OS              "zephyr"', out)

    def test_no_carrier_skips_carrier_macros(self) -> None:
        """When board.yaml omits the carrier block the loader must
        emit a clean header that only carries the SoM identifiers --
        no empty ALP_HW_BUILD_CARRIER_* macros."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                schema_version: 1
                som:
                  sku: E1M-AEN701
                os: zephyr
            """)
            rv = _run_loader(input_path=path, emit="hw-info-h")
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            self.assertIn('ALP_HW_BUILD_SOM_SKU', rv.stdout)
            self.assertNotIn('ALP_HW_BUILD_CARRIER_NAME', rv.stdout)

    def test_explicit_hw_rev_overrides_default(self) -> None:
        """Explicit som.hw_rev wins over the SKU preset's
        default_hw_rev (matching the loader's wider behaviour)."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                schema_version: 1
                som:
                  sku: E1M-AEN701
                  hw_rev: r1
                os: zephyr
            """)
            rv = _run_loader(input_path=path, emit="hw-info-h")
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            self.assertIn('ALP_HW_BUILD_SOM_HW_REV      "r1"', rv.stdout)


if __name__ == "__main__":
    unittest.main()
