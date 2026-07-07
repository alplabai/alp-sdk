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
from unittest import mock
from pathlib import Path


REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

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

    def test_peripheral_kconfig_registry_is_shared(self) -> None:
        """alp_project and alp_orchestrate consume one metadata registry."""
        import alp_project
        import alp_registries
        from alp_orchestrate.slugs import _PERIPHERAL_KCONFIG as orchestrate_map

        registry_map = alp_registries.peripheral_kconfig()
        self.assertEqual(alp_project._PERIPHERAL_KCONFIG, registry_map)
        self.assertEqual(orchestrate_map, registry_map)
        self.assertEqual(registry_map["uart"], "SERIAL")

    def test_peripheral_kconfig_registry_schema_is_gated(self) -> None:
        """validate_metadata rejects malformed peripheral registry data."""
        import validate_metadata

        with tempfile.TemporaryDirectory() as td:
            bad = Path(td) / "peripheral-kconfig.json"
            bad.write_text("""
                {
                  "schemaVersion": "peripheral-kconfig-v1",
                  "peripherals": {
                    "uart": "SERIAL",
                    "bad-token": "not_upper"
                  }
                }
            """, encoding="utf-8")
            with mock.patch.object(validate_metadata, "REPO", Path(td)), \
                 mock.patch.object(validate_metadata, "PERIPHERAL_KCONFIG_REGISTRY", bad):
                failures = validate_metadata._check_peripheral_kconfig()
        self.assertTrue(failures)
        self.assertIn("peripherals", failures[0][1][0])

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
        """A board.yaml with just `som:` + a single core should be
        accepted (board declaration is optional in the schema)."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-AEN801
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
            """)
            rv = _run_loader(input_path=path)
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            self.assertIn("CONFIG_ALP_SDK=y", rv.stdout)
            self.assertIn("CONFIG_ALP_SOC_ALIF_ENSEMBLE_E8=y", rv.stdout)

    def test_bad_sku_pattern_fails_schema(self) -> None:
        """The schema enforces the E1M-(AEN|V2N|V2M|NX9)... SKU pattern;
        an arbitrary string must fail schema validation with a non-zero
        exit.  The loader surfaces schema violations via
        OrchestratorError ('schema validation failed' or
        'does not match' in the message)."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: NOT-A-REAL-SKU
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
            """)
            # Pick an emit mode that hits the v2 loader (schema validation
            # is part of load_board_yaml).  system-manifest is the
            # cheapest non-Kconfig emit.
            rv = _run_loader(input_path=path, emit="system-manifest")
            self.assertNotEqual(rv.returncode, 0)
            lower = rv.stderr.lower()
            self.assertTrue(
                "schema violation" in lower
                or "schema validation failed" in lower
                or "does not match" in lower,
                msg=f"expected schema-violation marker; got: {rv.stderr}",
            )

    def test_unknown_sku_with_valid_pattern_fails_preset_lookup(self) -> None:
        """A SKU that matches the pattern but has no preset on disk
        must produce a clear error (either a rich ALP-B005 diagnostic
        from the validator or a legacy 'no preset' message from the
        orchestrator -- both are acceptable)."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-NX9999
                cores:
                  m33:
                    os: zephyr
                    app: ./src
            """)
            rv = _run_loader(input_path=path)
            self.assertNotEqual(rv.returncode, 0)
            lower = rv.stderr.lower()
            self.assertTrue(
                "no preset" in lower or "alp-b005" in lower,
                msg=f"expected 'no preset' or 'ALP-B005' in stderr; got: {rv.stderr}",
            )

    def test_inline_populated_flips_chip_kconfig(self) -> None:
        """An inline `populated:` block in a project's board.yaml
        produces the matching CONFIG_ALP_SDK_CHIP_* entries.  This is
        the customer path: one board.yaml, fully self-contained, no
        `preset:` reference."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                name: test-board
                som:
                  sku: E1M-AEN801
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
                populated:
                  bme280: true
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
                som:
                  sku: E1M-AEN801
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
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
                        som:
                          sku: E1M-AEN801
                        cores:
                          m55_hp:
                            os: zephyr
                            app: ./src
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
                        som:
                          sku: E1M-AEN801
                        diagnostics:
                          log_level: {log_level}
                        cores:
                          m55_hp:
                            os: zephyr
                            app: ./src
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
        self.assertIn('#define ALP_HW_BUILD_SOM_SKU         "E1M-AEN801"', out)
        self.assertIn('#define ALP_HW_BUILD_SOM_FAMILY      "aen"', out)
        self.assertIn('#define ALP_HW_BUILD_SOM_HW_REV      "r2"', out)
        self.assertIn('#define ALP_HW_BUILD_BOARD_NAME      "E1M-EVK"', out)
        self.assertIn('#define ALP_HW_BUILD_BOARD_HW_REV    "r1"', out)
        self.assertIn('#define ALP_HW_BUILD_OS              "zephyr"', out)

    def test_no_board_skips_board_macros(self) -> None:
        """When board.yaml omits the board block the loader must
        emit a clean header that only carries the SoM identifiers --
        no empty ALP_HW_BUILD_BOARD_* macros."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-AEN801
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
            """)
            rv = _run_loader(input_path=path, emit="hw-info-h")
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            self.assertIn('ALP_HW_BUILD_SOM_SKU', rv.stdout)
            self.assertNotIn('ALP_HW_BUILD_BOARD_NAME', rv.stdout)

    def test_explicit_hw_rev_overrides_default(self) -> None:
        """Explicit som.hw_rev wins over the SKU preset's
        default_hw_rev (matching the loader's wider behaviour)."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-AEN801
                  hw_rev: r1
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
            """)
            rv = _run_loader(input_path=path, emit="hw-info-h")
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            self.assertIn('ALP_HW_BUILD_SOM_HW_REV      "r1"', rv.stdout)


class TestWestLibrariesEmit(unittest.TestCase):
    """`--emit west-libraries` mode -- emits a west.yml
    name-allowlist fragment from board.yaml's `libraries:` array."""

    def test_template_emits_module_lines(self) -> None:
        rv = _run_loader(input_path=TEMPLATE, emit="west-libraries")
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        out = rv.stdout
        self.assertIn("manifest:", out)
        self.assertIn("name-allowlist:", out)
        # Template has lvgl + mbedtls + cmsis_dsp + etl in libraries.
        # First three are Zephyr modules; etl is a header-only library
        # the loader's profile path covers, so it should land in the
        # commented "not-a-Zephyr-module" tail, not the allowlist.
        self.assertIn("- lvgl", out)
        self.assertIn("- mbedtls", out)
        self.assertIn("- cmsis-dsp", out)
        tail = out.split("not Zephyr modules today")[-1]
        self.assertIn("etl", tail)

    def test_empty_libraries_emits_well_formed_empty_block(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-AEN801
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
            """)
            rv = _run_loader(input_path=path, emit="west-libraries")
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            self.assertIn("name-allowlist:", rv.stdout)
            self.assertIn("[]", rv.stdout)

    def test_top_level_cloud_libraries_emit_exact_west_projects(self) -> None:
        """ADR 0018 top-level libraries can carry their own west project pins
        when Zephyr's own west.yml does not import the upstream repo."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                som:
                  sku: E1M-V2N101
                libraries: [aws-iot, azure-iot]
                cores:
                  m33_sm:
                    os: zephyr
                    app: ./src
            """)
            rv = _run_loader(input_path=path, emit="west-libraries")
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            out = rv.stdout
            self.assertIn("name: aws-iot-device-sdk-embedded-C", out)
            self.assertIn("url: https://github.com/aws/aws-iot-device-sdk-embedded-C.git", out)
            self.assertIn("revision: v3.1.5", out)
            self.assertIn("path: modules/lib/aws-iot-device-sdk-embedded-C", out)
            self.assertIn("name: azure-sdk-for-c", out)
            self.assertIn("url: https://github.com/Azure/azure-sdk-for-c.git", out)
            self.assertIn("revision: 1.5.0", out)
            self.assertIn("path: modules/lib/azure-sdk-for-c", out)


class TestValidatorPeripheralCheck(unittest.TestCase):
    """validate_board_yaml.py end-to-end smoke test on a shipped v2
    example.  Under v2 the per-core `peripherals:` list lives inside
    `cores.<id>.peripherals:` rather than at project scope; the
    validator's schema + preset + hw_rev checks must still pass
    cleanly on the canonical adc-voltmeter example."""

    def test_real_example_passes(self) -> None:
        """The shipped adc-voltmeter example declares
        `cores.m55_hp.peripherals: [adc]` against E1M-AEN801
        (alif:ensemble:e8, the lead part).  Schema + preset + hw_rev
        checks must all be green; the SoM preset's partial_hw_config
        flag means the validator exits 0 with a 'clean (with warnings)'
        tail."""
        example = REPO / "examples" / "peripheral-io" / "adc-voltmeter" / "board.yaml"
        rv = subprocess.run(
            [sys.executable,
             str(REPO / "scripts" / "validate_board_yaml.py"),
             "--input", str(example)],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        self.assertIn(f"OK   schema:", rv.stdout)
        self.assertIn("OK   board preset: e1m-evk", rv.stdout)
        self.assertIn("OK   som E1M-AEN801 hw_rev:", rv.stdout)


class TestHwBackendsLoader(unittest.TestCase):
    """§D.lib.loader cross-library HW-backend wiring.

    For each SoM SKU + a representative library subset, assert the
    loader emits the expected per-NPU / per-accelerator
    `CONFIG_ALP_*=y` lines.  Locks in the per-SKU wiring so future
    metadata changes (new caps, new silicon families) can't
    silently drop bindings.
    """

    LIBS = [
        "tflite_micro", "lvgl", "mbedtls", "cmsis_dsp",
        "littlefs", "bearssl", "madgwick_ahrs", "u8g2",
        "gfx_compat", "minimp3", "opus", "libhelix",
    ]

    _SKU_CORE: dict[str, str] = {
        "AEN": "m55_hp",
        "V2N": "m33_sm",
        "V2M": "m33_sm",
        "NX9": "m33",
    }

    @classmethod
    def _core_for_sku(cls, sku: str) -> str:
        for prefix, core in cls._SKU_CORE.items():
            if f"E1M-{prefix}" in sku:
                return core
        return "m55_hp"

    @classmethod
    def _emit(cls, sku: str) -> str:
        """Run the full loader for `sku` + every library, return stdout."""
        core = cls._core_for_sku(sku)
        libs_yaml = "".join(f"              - {lib}\n" for lib in cls.LIBS)
        body = (

            "som:\n"
            f"  sku: {sku}\n"
            "cores:\n"
            f"  {core}:\n"
            "    os: zephyr\n"
            "    app: ./src\n"
            "    libraries:\n"
            f"{libs_yaml}"
        )
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "board.yaml"
            path.write_text(body, encoding="utf-8")
            rv = subprocess.run(
                [sys.executable, str(LOADER),
                 "--input", str(path),
                 "--emit", "zephyr-conf",
                 "--core", core],
                capture_output=True, text=True, check=False,
            )
        # In any subTest we want the actual returncode + stderr in the
        # failure message, so attach them to the returned string.
        return rv.stdout if rv.returncode == 0 else f"FAIL rc={rv.returncode}: {rv.stderr}\n{rv.stdout}"

    def assertEmitted(self, sku: str, kconfig: str) -> None:  # noqa: N802
        out = self._emit(sku)
        self.assertIn(kconfig, out, msg=f"{sku}: missing {kconfig}\n{out}")

    def assertNotEmitted(self, sku: str, kconfig: str) -> None:  # noqa: N802
        out = self._emit(sku)
        self.assertNotIn(kconfig, out, msg=f"{sku}: unexpected {kconfig}\n{out}")

    # --- per-SKU expectations ---------------------------------------

    def test_aen301_u55_only_no_u85(self) -> None:
        """E3 carries two U55s, no U85; family-wide primary is U55."""
        self.assertEmitted    ("E1M-AEN301", "CONFIG_ALP_TFLM_ETHOS_U55=y")
        self.assertNotEmitted ("E1M-AEN301", "CONFIG_ALP_TFLM_ETHOS_U85=y")
        self.assertEmitted    ("E1M-AEN301", "CONFIG_ALP_TFLM_HELIUM=y")
        self.assertNotEmitted ("E1M-AEN301", "CONFIG_ALP_TFLM_NEON=y")

    def test_aen401_u85_primary_plus_u55_secondary(self) -> None:
        """E4 = 2x U55 + 1x U85; both driver shims must be linked."""
        self.assertEmitted ("E1M-AEN401", "CONFIG_ALP_TFLM_ETHOS_U85=y")
        self.assertEmitted ("E1M-AEN401", "CONFIG_ALP_TFLM_ETHOS_U55=y")
        # E4 has no A32 -> no Neon.
        self.assertEmitted    ("E1M-AEN401", "CONFIG_ALP_TFLM_HELIUM=y")
        self.assertNotEmitted ("E1M-AEN401", "CONFIG_ALP_TFLM_NEON=y")

    def test_aen601_gpu2d_present(self) -> None:
        """E6 has GPU2D + DAVE2D; LVGL picks GPU2D priority 1."""
        self.assertEmitted ("E1M-AEN601", "CONFIG_ALP_TFLM_ETHOS_U85=y")
        self.assertEmitted ("E1M-AEN601", "CONFIG_ALP_TFLM_ETHOS_U55=y")
        self.assertEmitted ("E1M-AEN601", "CONFIG_ALP_LVGL_GPU2D=y")
        self.assertEmitted ("E1M-AEN601", "CONFIG_ALP_GFX_COMPAT_GPU2D=y")
        # E6 has A32 -> Neon present for cmsis_dsp / opus / minimp3.
        # NEON entries on those libraries are after HELIUM, and the
        # loader's per-class first-match picks HELIUM on Ensemble.
        self.assertEmitted ("E1M-AEN601", "CONFIG_ALP_TFLM_HELIUM=y")

    def test_aen801_hexspi_resolves_xspi_dma(self) -> None:
        """E8 uses HexSPI not OctalSPI; the second priority entry
        (requires_cap: hexspi_dma) must still resolve the same
        CONFIG_ALP_LITTLEFS_XSPI_DMA driver shim."""
        self.assertEmitted ("E1M-AEN801", "CONFIG_ALP_LITTLEFS_XSPI_DMA=y")

    def test_v2n101_drp_ai_plus_cau(self) -> None:
        """V2N101: no Ethos, primary NPU is DRP-AI; mbedtls/bearssl
        route through GD32 bridge CAU."""
        self.assertEmitted    ("E1M-V2N101", "CONFIG_ALP_TFLM_DRP_AI=y")
        self.assertNotEmitted ("E1M-V2N101", "CONFIG_ALP_TFLM_ETHOS_U55=y")
        self.assertNotEmitted ("E1M-V2N101", "CONFIG_ALP_TFLM_ETHOS_U85=y")
        self.assertEmitted    ("E1M-V2N101", "CONFIG_ALP_TFLM_NEON=y")
        self.assertEmitted    ("E1M-V2N101", "CONFIG_ALP_MBEDTLS_CAU=y")
        self.assertEmitted    ("E1M-V2N101", "CONFIG_ALP_BEARSSL_CAU=y")
        self.assertEmitted    ("E1M-V2N101", "CONFIG_ALP_LITTLEFS_EMMC_DMA=y")
        self.assertNotEmitted ("E1M-V2N101", "CONFIG_ALP_MBEDTLS_CRYPTOCELL=y")
        # Regression guard: the cmsis_dsp profile dir must equal its
        # board.yaml token so the loader's profile-path lookup resolves
        # and the HW-accelerator bindings are not silently dropped.
        self.assertEmitted    ("E1M-V2N101", "CONFIG_ALP_CMSIS_DSP_NEON=y")
        self.assertEmitted    ("E1M-V2N101", "CONFIG_ALP_CMSIS_DSP_TMU_CORDIC=y")
        self.assertEmitted    ("E1M-V2N101", "CONFIG_ALP_CMSIS_DSP_TMU_FFT=y")

    def test_nx9101_u65_resolves(self) -> None:
        """NX9101: i.MX 93's Ethos-U65 must resolve via the
        ml_npu_primary class; the legacy preferred_backend handler
        + the new loader hook must both contribute their gates."""
        self.assertEmitted    ("E1M-NX9101", "CONFIG_ALP_TFLM_ETHOS_U65=y")
        self.assertNotEmitted ("E1M-NX9101", "CONFIG_ALP_TFLM_ETHOS_U55=y")
        self.assertNotEmitted ("E1M-NX9101", "CONFIG_ALP_TFLM_ETHOS_U85=y")
        self.assertEmitted    ("E1M-NX9101", "CONFIG_ALP_TFLM_NEON=y")

    def test_universal_fallback_dma_always_emitted(self) -> None:
        """The unconditional DMA fallback (tensor_dma_copy /
        i2s_dma / spi_dma) must fire on every SKU because none of
        them carries a `requires_cap:` matcher."""
        for sku in ("E1M-AEN301", "E1M-AEN801", "E1M-V2N101", "E1M-NX9101"):
            with self.subTest(sku=sku):
                self.assertEmitted (sku, "CONFIG_ALP_TFLM_DMA_COPY=y")
                self.assertEmitted (sku, "CONFIG_ALP_MINIMP3_I2S_DMA=y")

    def test_optiga_truth_cross_family(self) -> None:
        """OPTIGA Trust M is populated on AEN + V2N + NX9101.  With
        `requires_cap: optiga_trust_m`, mbedtls/bearssl's OPTIGA
        gate fires across all three families -- but only when no
        higher-priority crypto accelerator (CryptoCell / Inline-AES
        / CAU) wins first.  AEN picks CryptoCell; NX9101 has no
        higher-priority crypto so OPTIGA fires there."""
        self.assertEmitted    ("E1M-AEN401", "CONFIG_ALP_MBEDTLS_CRYPTOCELL=y")
        self.assertNotEmitted ("E1M-AEN401", "CONFIG_ALP_MBEDTLS_OPTIGA=y")
        self.assertEmitted    ("E1M-NX9101", "CONFIG_ALP_MBEDTLS_OPTIGA=y")
        self.assertNotEmitted ("E1M-NX9101", "CONFIG_ALP_MBEDTLS_CRYPTOCELL=y")

    def test_sw_fallback_always_emitted(self) -> None:
        """Each library's SW-fallback CONFIG_*=y is emitted
        unconditionally via _LIBRARY_KCONFIG (separate from the
        hw-backends loader).  Both new §D.lib libraries and the 4
        baseline ones (lvgl / mbedtls / cmsis_dsp / littlefs) emit
        their fallback knob alongside the upstream library knob."""
        out = self._emit("E1M-AEN401")
        for fallback in (
            # §D.lib new libraries
            "CONFIG_ALP_TFLM_REF_KERNELS=y",
            "CONFIG_ALP_BEARSSL_PURE_C=y",
            "CONFIG_ALP_OPUS_PURE_C=y",
            "CONFIG_ALP_MINIMP3_PURE_C=y",
            "CONFIG_ALP_LIBHELIX_PURE_C=y",
            "CONFIG_ALP_MADGWICK_LIBM=y",
            "CONFIG_ALP_U8G2_SW_BLIT=y",
            "CONFIG_ALP_GFX_COMPAT_SW=y",
            # Baseline libs (added explicit emission in the §D.lib
            # follow-up audit).
            "CONFIG_ALP_LVGL_SW_BLIT=y",
            "CONFIG_ALP_MBEDTLS_PURE_C=y",
            "CONFIG_ALP_CMSIS_DSP_SCALAR=y",
            "CONFIG_ALP_LITTLEFS_SYNC_IO=y",
        ):
            with self.subTest(fallback=fallback):
                self.assertIn(fallback, out)


class TestInferenceFromSomCaps(unittest.TestCase):
    """Inference CONFIGs are emitted from the SoM preset's
    `capabilities:` matrix, NEVER from board.yaml.  Captures the
    2026-05-16 schema tightening: `inference.backend` was a footgun
    (let customers pick backends incompatible with the silicon, and
    duplicated a fact the SoM preset already encoded).  See
    feedback_silicon_determined_fields_not_customer_facing.md.

    The runtime API (`alp_inference_open(.backend = ...)`) still
    lets apps pick per-handle for concurrent multi-NPU dispatch on
    multi-accelerator SKUs (V2M101 = DRP-AI3 + DEEPX DX-M1); the
    build wires in EVERY backend the SoM has so the runtime pick
    always finds a compiled dispatcher.
    """

    def _v2_zephyr_slice(self, sku: str, core: str) -> tuple[int, str, str]:
        body = f"""
            som:
              sku: {sku}
            cores:
              {core}:
                os: zephyr
                app: ./src
        """
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), body)
            rv = subprocess.run(
                [sys.executable, str(LOADER),
                 "--input", str(path),
                 "--emit", "zephyr-conf",
                 "--core", core],
                capture_output=True, text=True, check=False,
            )
        return rv.returncode, rv.stdout, rv.stderr

    def _v2_cmake_slice(self, sku: str, core: str, os_: str) -> tuple[int, str, str]:
        body = f"""
            som:
              sku: {sku}
            cores:
              {core}:
                os: {os_}
                app: ./src
        """
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), body)
            rv = subprocess.run(
                [sys.executable, str(LOADER),
                 "--input", str(path),
                 "--emit", "cmake-args",
                 "--core", core],
                capture_output=True, text=True, check=False,
            )
        return rv.returncode, rv.stdout, rv.stderr

    # --- Zephyr emit: SoM caps drive inference CONFIGs ----------------

    def test_aen701_zephyr_emits_tflm_plus_ethos_u(self) -> None:
        """E1M-AEN701 ships 2x Ethos-U55 (no U85, no DRP-AI, no DEEPX).
        SoM caps drive the inference CONFIGs; board.yaml never asked."""
        rc, out, err = self._v2_zephyr_slice("E1M-AEN701", "m55_hp")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn("CONFIG_ALP_SDK_INFERENCE_BACKEND_TFLM=y", out)
        self.assertIn("CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_AEN=y", out)
        self.assertNotIn("DRPAI", out)

    def test_v2n101_zephyr_emits_tflm_only(self) -> None:
        """E1M-V2N101 M33 slice gets TFLM only: the DRP-AI3 engine is
        A55/Linux-side (MERA runtime), so no Zephyr inference backend
        Kconfig exists for it (issues #58/#59)."""
        rc, out, err = self._v2_zephyr_slice("E1M-V2N101", "m33_sm")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn("CONFIG_ALP_SDK_INFERENCE_BACKEND_TFLM=y", out)
        self.assertNotIn("DRPAI", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_AEN=y", out)

    def test_v2m101_zephyr_emits_tflm_only(self) -> None:
        """E1M-V2M101 = V2N + DEEPX.  Neither engine is Zephyr-side
        (DRP-AI3 = A55 MERA runtime, DX-M1 = A55 PCIe libdxrt); the
        Yocto emit path carries the concurrent-NPU plumbing."""
        rc, out, err = self._v2_zephyr_slice("E1M-V2M101", "m33_sm")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn("CONFIG_ALP_SDK_INFERENCE_BACKEND_TFLM=y", out)
        self.assertNotIn("DRPAI", out)
        # The DX-M1 *chip driver* (CONFIG_ALP_SDK_CHIP_DEEPX_DXM1 --
        # M33-side management, not inference) legitimately stays; only
        # the inference-backend namespace must be DEEPX-free.
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_BACKEND_DEEPX", out)

    # --- G-1 + G-2 -- per-variant Ethos-U + per-CPU-class TFLM ------------
    #
    # The orchestrator's inference-dispatcher block is silicon-determined
    # (it never reads board.yaml) so swapping `som.sku:` between two SoMs
    # carrying different NPU populations or different CPU classes must
    # update the emitted CONFIG_ALP_SDK_INFERENCE_* switches accordingly.
    # Pre-2026-05-18 the emit was variant-blind: AEN401 (U85-carrying)
    # generated the same alp.conf as AEN701 (U55-only), and the M55_HP
    # slice's TFLM kernel set wasn't distinguishable from the M33_SM
    # baseline.  The tests below lock the post-fix behaviour.

    def test_aen701_emits_u55_only_no_u85(self) -> None:
        """E7 carries two U55s, no U85; only the U55 switch fires."""
        rc, out, err = self._v2_zephyr_slice("E1M-AEN701", "m55_hp")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U55=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U85=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U65=y", out)

    def test_aen801_emits_both_u55_and_u85(self) -> None:
        """E8 carries 2x U55 + 1x U85; BOTH variant switches must fire so
        the Arm Ethos-U driver compiles both kernel sets in."""
        rc, out, err = self._v2_zephyr_slice("E1M-AEN801", "m55_hp")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U55=y", out)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U85=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U65=y", out)

    def test_aen401_emits_both_u55_and_u85(self) -> None:
        """E4 same family pattern as E8: 2x U55 + 1x U85."""
        rc, out, err = self._v2_zephyr_slice("E1M-AEN401", "m55_hp")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U55=y", out)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U85=y", out)

    def test_nx9101_emits_u65_plus_n93(self) -> None:
        """i.MX 93 carries a single Ethos-U65; the new U65 switch and
        the legacy N93 PHY-side switch coexist (orthogonal selectors)."""
        rc, out, err = self._v2_zephyr_slice("E1M-NX9101", "m33")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U65=y", out)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_N93=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U55=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U85=y", out)

    def test_v2n101_emits_no_ethos_variants(self) -> None:
        """V2N has no Ethos-U at all; none of the per-variant switches
        fire (and no DRP-AI Kconfig exists -- A55-side engine)."""
        rc, out, err = self._v2_zephyr_slice("E1M-V2N101", "m33_sm")
        self.assertEqual(rc, 0, msg=err)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U55=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U65=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U85=y", out)

    def test_aen701_m55_emits_tflm_helium(self) -> None:
        """M55_HP on E7 -- ARMv8.1-M Helium -> the orchestrator must
        emit the HELIUM kernel selector (not NEON, not REF)."""
        rc, out, err = self._v2_zephyr_slice("E1M-AEN701", "m55_hp")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_HELIUM=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_NEON=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_REF=y", out)

    def test_v2n101_m33_emits_tflm_ref(self) -> None:
        """M33_SM on V2N101 -- baseline ARMv8-M without Helium / DSP ->
        the orchestrator must fall back to the REF kernel selector."""
        rc, out, err = self._v2_zephyr_slice("E1M-V2N101", "m33_sm")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_REF=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_NEON=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_HELIUM=y", out)

    def test_nx9101_m33_emits_tflm_ref(self) -> None:
        """M33 on i.MX 93 -- baseline ARMv8-M, single-precision FPU,
        no MVE -> REF."""
        rc, out, err = self._v2_zephyr_slice("E1M-NX9101", "m33")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_REF=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_TFLM_KERNEL_HELIUM=y", out)

    # --- cmake-args / Yocto emit: concurrent multi-NPU on V2M101 ------

    def test_v2m101_baremetal_cmake_args_enable_both_npus(self) -> None:
        """V2M101 must compile both DRP-AI3 and DEEPX DX-M1 dispatchers
        in -- concurrent independent models (e.g. m_vision on DEEPX,
        m_audio on DRP-AI) is the whole point of the V2N+DEEPX
        co-package.  Pre-2026-05-16 the loader picked ONE backend and
        the second `alp_inference_open()` call would fail NOSUPPORT."""
        rc, out, err = self._v2_cmake_slice(
            "E1M-V2M101", "a55_cluster", "baremetal")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn("ALP_SDK_USE_DRPAI_V2N=ON", out)
        self.assertIn("ALP_SDK_USE_DEEPX_DXM1=ON", out)

    def test_v2n101_baremetal_cmake_args_drpai_only(self) -> None:
        """V2N101 has DRP-AI3 but no DEEPX -- the cmake-args emit
        must NOT include the DEEPX flag, even though V2M101 (same
        family) does."""
        rc, out, err = self._v2_cmake_slice(
            "E1M-V2N101", "a55_cluster", "baremetal")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn("ALP_SDK_USE_DRPAI_V2N=ON", out)
        self.assertNotIn("ALP_SDK_USE_DEEPX_DXM1=ON", out)

    # --- Schema-level rejection of inference.backend ------------------

    def test_inference_backend_field_rejected_by_schema(self) -> None:
        """board.yaml cannot declare `inference.backend` -- silicon
        capability is not a project-level choice.  Schema's
        additionalProperties: false rejects the unknown property at
        validation time."""
        body = """
            som:
              sku: E1M-AEN801
            cores:
              m55_hp:
                os: zephyr
                app: ./src
                inference:
                  backend: ethos_u
        """
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), body)
            rv = _run_loader(input_path=path, emit="system-manifest")
        self.assertNotEqual(rv.returncode, 0,
                            msg=f"schema accepted inference.backend; stdout={rv.stdout}")
        # The error should mention `backend` so the customer knows what
        # to delete from their board.yaml.
        self.assertIn("backend", rv.stderr.lower(),
                      msg=f"error must name the offending field; got: {rv.stderr}")

    def test_default_arena_kib_still_allowed(self) -> None:
        """`default_arena_kib` is genuinely app-level tuning (per-model
        memory budget) and stays as a per-core knob."""
        body = """
            som:
              sku: E1M-AEN801
            cores:
              m55_hp:
                os: zephyr
                app: ./src
                inference:
                  default_arena_kib: 256
        """
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), body)
            rv = _run_loader(input_path=path, emit="system-manifest")
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)


class TestAlpBoardDefineEmit(unittest.TestCase):
    """ALP_BOARD_<SLUG> compile define is emitted automatically from
    the board preset name -- the build-system hook that makes
    <alp/board.h> resolve on real/west/native builds without
    per-testcase extra_args.

    Tests are named so `-k alp_board` selects the entire class.

    Slug derivation: board_name.lower().replace("-", "_").upper()
      "E1M-X-EVK" -> "E1M_X_EVK" -> define ALP_BOARD_E1M_X_EVK
      "E1M-EVK"   -> "E1M_EVK"   -> define ALP_BOARD_E1M_EVK
    """

    def _cmake_args(self, sku: str, preset: str, core: str, os_: str) -> tuple[int, str, str]:
        """Run --emit cmake-args for a board.yaml with a named preset."""
        body = f"""
            preset: {preset}
            som:
              sku: {sku}
            cores:
              {core}:
                os: {os_}
                app: ./src
        """
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), body)
            rv = subprocess.run(
                [sys.executable, str(LOADER),
                 "--input", str(path),
                 "--emit", "cmake-args",
                 "--core", core],
                capture_output=True, text=True, check=False,
            )
        return rv.returncode, rv.stdout, rv.stderr

    def _zephyr_conf(self, sku: str, preset: str, core: str) -> tuple[int, str, str]:
        """Run --emit zephyr-conf for a board.yaml with a named preset."""
        body = f"""
            preset: {preset}
            som:
              sku: {sku}
            cores:
              {core}:
                os: zephyr
                app: ./src
        """
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), body)
            rv = subprocess.run(
                [sys.executable, str(LOADER),
                 "--input", str(path),
                 "--emit", "zephyr-conf",
                 "--core", core],
                capture_output=True, text=True, check=False,
            )
        return rv.returncode, rv.stdout, rv.stderr

    # --- cmake-args path ---

    def test_alp_board_cmake_args_e1m_x_evk(self) -> None:
        """cmake-args for preset e1m-x-evk must emit -DALP_BOARD_E1M_X_EVK."""
        rc, out, err = self._cmake_args(
            "E1M-V2N101", "e1m-x-evk", "a55_cluster", "baremetal")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn("-DALP_BOARD_E1M_X_EVK", out,
                      msg=f"ALP_BOARD_E1M_X_EVK missing from cmake-args:\n{out}")

    def test_alp_board_cmake_args_e1m_evk(self) -> None:
        """cmake-args for preset e1m-evk must emit -DALP_BOARD_E1M_EVK."""
        rc, out, err = self._cmake_args(
            "E1M-AEN801", "e1m-evk", "a32_cluster", "baremetal")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn("-DALP_BOARD_E1M_EVK", out,
                      msg=f"ALP_BOARD_E1M_EVK missing from cmake-args:\n{out}")

    def test_alp_board_cmake_args_no_define_without_preset_name(self) -> None:
        """cmake-args for an inline board (no preset name) must NOT emit
        any ALP_BOARD_* define -- the guard is `if project.board_name`."""
        body = """
            som:
              sku: E1M-AEN801
            cores:
              m55_hp:
                os: baremetal
                app: ./src
        """
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), body)
            rv = subprocess.run(
                [sys.executable, str(LOADER),
                 "--input", str(path),
                 "--emit", "cmake-args",
                 "--core", "m55_hp"],
                capture_output=True, text=True, check=False,
            )
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        self.assertNotIn("ALP_BOARD_", rv.stdout,
                         msg="ALP_BOARD_* must not appear for a nameless board")

    # --- zephyr-conf path ---

    def test_alp_board_zephyr_conf_e1m_evk(self) -> None:
        """zephyr-conf for preset e1m-evk must include ALP_BOARD_E1M_EVK in
        CONFIG_COMPILER_OPT so the facade resolves on every Zephyr build."""
        rc, out, err = self._zephyr_conf(
            "E1M-AEN801", "e1m-evk", "m55_hp")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn('CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_EVK"', out,
                      msg=f"exact CONFIG_COMPILER_OPT line missing from zephyr-conf:\n{out}")

    def test_alp_board_zephyr_conf_e1m_x_evk(self) -> None:
        """zephyr-conf for preset e1m-x-evk must include ALP_BOARD_E1M_X_EVK."""
        rc, out, err = self._zephyr_conf(
            "E1M-V2N101", "e1m-x-evk", "m33_sm")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn('CONFIG_COMPILER_OPT="-DALP_BOARD_E1M_X_EVK"', out,
                      msg=f"exact CONFIG_COMPILER_OPT line missing from zephyr-conf:\n{out}")

    def test_alp_board_zephyr_conf_no_define_without_preset_name(self) -> None:
        """zephyr-conf for an inline board (no preset name) must NOT emit
        CONFIG_COMPILER_OPT with ALP_BOARD_* -- guarded on board_name."""
        body = """
            som:
              sku: E1M-AEN801
            cores:
              m55_hp:
                os: zephyr
                app: ./src
        """
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), body)
            rv = subprocess.run(
                [sys.executable, str(LOADER),
                 "--input", str(path),
                 "--emit", "zephyr-conf",
                 "--core", "m55_hp"],
                capture_output=True, text=True, check=False,
            )
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        self.assertNotIn("ALP_BOARD_", rv.stdout,
                         msg="ALP_BOARD_* must not appear for a nameless board")


if __name__ == "__main__":
    unittest.main()
