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
        """A board.yaml with just schema_version + som + a single core
        should be accepted (carrier is optional in the schema)."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                schema_version: 2
                som:
                  sku: E1M-AEN701
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
            """)
            rv = _run_loader(input_path=path)
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            self.assertIn("CONFIG_ALP_SDK=y", rv.stdout)
            self.assertIn("CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y", rv.stdout)

    def test_bad_sku_pattern_fails_schema(self) -> None:
        """The schema enforces the E1M-(AEN|V2N|V2M|NX9)... SKU pattern;
        an arbitrary string must fail v2 schema validation with a
        non-zero exit.  v2 inputs route through the orchestrator
        loader, which surfaces schema violations via OrchestratorError;
        the test accepts either the v1-style 'schema violation' or the
        v2 'schema validation failed' phrasing."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                schema_version: 2
                som:
                  sku: NOT-A-REAL-SKU
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
            """)
            # Pick an emit mode that hits the v2 loader (schema validation
            # is part of load_board_yaml).  system-manifest is the
            # cheapest v2-only emit.
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
        must produce a clear missing-preset error, not a schema error."""
        with tempfile.TemporaryDirectory() as td:
            path = _write_board(Path(td), """
                schema_version: 2
                som:
                  sku: E1M-NX9999
                cores:
                  m33:
                    os: zephyr
                    app: ./src
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
                schema_version: 2
                som:
                  sku: E1M-AEN701
                carrier:
                  name: E1M-EVK
                  populated:
                    bme280: true
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
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
                schema_version: 2
                som:
                  sku: E1M-AEN701
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
                        schema_version: 2
                        som:
                          sku: E1M-AEN701
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
                        schema_version: 2
                        som:
                          sku: E1M-AEN701
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
                schema_version: 2
                som:
                  sku: E1M-AEN701
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
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
                schema_version: 2
                som:
                  sku: E1M-AEN701
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
                schema_version: 2
                som:
                  sku: E1M-AEN701
                cores:
                  m55_hp:
                    os: zephyr
                    app: ./src
            """)
            rv = _run_loader(input_path=path, emit="west-libraries")
            self.assertEqual(rv.returncode, 0, msg=rv.stderr)
            self.assertIn("name-allowlist:", rv.stdout)
            self.assertIn("[]", rv.stdout)


class TestValidatorPeripheralCheck(unittest.TestCase):
    """validate_board_yaml.py end-to-end smoke test on a shipped v2
    example.  Under v2 the per-core `peripherals:` list lives inside
    `cores.<id>.peripherals:` rather than at project scope; the
    validator's schema + preset + hw_rev checks must still pass
    cleanly on the canonical adc-voltmeter example."""

    def test_real_example_passes(self) -> None:
        """The shipped adc-voltmeter example declares
        `cores.m55_hp.peripherals: [adc]` against E1M-AEN701
        (alif:ensemble:e7).  Schema + preset + hw_rev checks must all
        be green; the SoM preset's partial_hw_config flag means the
        validator exits 0 with a 'clean (with warnings)' tail."""
        example = REPO / "examples" / "adc-voltmeter" / "board.yaml"
        rv = subprocess.run(
            [sys.executable,
             str(REPO / "scripts" / "validate_board_yaml.py"),
             "--input", str(example)],
            capture_output=True, text=True, check=False,
        )
        self.assertEqual(rv.returncode, 0, msg=rv.stderr)
        self.assertIn(f"OK   schema:", rv.stdout)
        self.assertIn("OK   carrier preset: E1M-EVK", rv.stdout)
        self.assertIn("OK   som E1M-AEN701 hw_rev:", rv.stdout)


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
            "schema_version: 2\n"
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
            schema_version: 2
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
            schema_version: 2
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
        self.assertIn("CONFIG_ALP_SDK_INFERENCE_TFLM=y", out)
        self.assertIn("CONFIG_ALP_SDK_INFERENCE_ETHOS_U=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_DRPAI=y", out)

    def test_v2n101_zephyr_emits_tflm_plus_drpai(self) -> None:
        """E1M-V2N101 has DRP-AI3 on V2N silicon, no Ethos, no DEEPX."""
        rc, out, err = self._v2_zephyr_slice("E1M-V2N101", "m33_sm")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn("CONFIG_ALP_SDK_INFERENCE_TFLM=y", out)
        self.assertIn("CONFIG_ALP_SDK_INFERENCE_DRPAI=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_ETHOS_U=y", out)

    def test_v2m101_zephyr_emits_tflm_plus_drpai(self) -> None:
        """E1M-V2M101 = V2N + DEEPX.  On Zephyr only DRPAI is wired
        (DEEPX is host-Linux-only via the PCIe driver); the Yocto
        emit path carries the concurrent-NPU plumbing."""
        rc, out, err = self._v2_zephyr_slice("E1M-V2M101", "m33_sm")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn("CONFIG_ALP_SDK_INFERENCE_TFLM=y", out)
        self.assertIn("CONFIG_ALP_SDK_INFERENCE_DRPAI=y", out)

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
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U55=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U85=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U65=y", out)

    def test_aen801_emits_both_u55_and_u85(self) -> None:
        """E8 carries 2x U55 + 1x U85; BOTH variant switches must fire so
        the Arm Ethos-U driver compiles both kernel sets in."""
        rc, out, err = self._v2_zephyr_slice("E1M-AEN801", "m55_hp")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U55=y", out)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U85=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U65=y", out)

    def test_aen401_emits_both_u55_and_u85(self) -> None:
        """E4 same family pattern as E8: 2x U55 + 1x U85."""
        rc, out, err = self._v2_zephyr_slice("E1M-AEN401", "m55_hp")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U55=y", out)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U85=y", out)

    def test_nx9101_emits_u65_plus_n93(self) -> None:
        """i.MX 93 carries a single Ethos-U65; the new U65 switch and
        the legacy N93 PHY-side switch coexist (orthogonal selectors)."""
        rc, out, err = self._v2_zephyr_slice("E1M-NX9101", "m33")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U65=y", out)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_N93=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U55=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U85=y", out)

    def test_v2n101_emits_no_ethos_variants(self) -> None:
        """V2N has no Ethos-U at all; none of the per-variant switches
        fire even though _DRPAI does."""
        rc, out, err = self._v2_zephyr_slice("E1M-V2N101", "m33_sm")
        self.assertEqual(rc, 0, msg=err)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U55=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U65=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U85=y", out)

    def test_aen701_m55_emits_tflm_helium(self) -> None:
        """M55_HP on E7 -- ARMv8.1-M Helium -> the orchestrator must
        emit the HELIUM kernel selector (not NEON, not REF)."""
        rc, out, err = self._v2_zephyr_slice("E1M-AEN701", "m55_hp")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_TFLM_HELIUM=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_TFLM_NEON=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_TFLM_REF=y", out)

    def test_v2n101_m33_emits_tflm_ref(self) -> None:
        """M33_SM on V2N101 -- baseline ARMv8-M without Helium / DSP ->
        the orchestrator must fall back to the REF kernel selector."""
        rc, out, err = self._v2_zephyr_slice("E1M-V2N101", "m33_sm")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_TFLM_REF=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_TFLM_NEON=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_TFLM_HELIUM=y", out)

    def test_nx9101_m33_emits_tflm_ref(self) -> None:
        """M33 on i.MX 93 -- baseline ARMv8-M, single-precision FPU,
        no MVE -> REF."""
        rc, out, err = self._v2_zephyr_slice("E1M-NX9101", "m33")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn   ("CONFIG_ALP_SDK_INFERENCE_TFLM_REF=y", out)
        self.assertNotIn("CONFIG_ALP_SDK_INFERENCE_TFLM_HELIUM=y", out)

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
        self.assertIn("ALP_SDK_USE_DRPAI=ON", out)
        self.assertIn("ALP_SDK_USE_DEEPX_DXM1=ON", out)

    def test_v2n101_baremetal_cmake_args_drpai_only(self) -> None:
        """V2N101 has DRP-AI3 but no DEEPX -- the cmake-args emit
        must NOT include the DEEPX flag, even though V2M101 (same
        family) does."""
        rc, out, err = self._v2_cmake_slice(
            "E1M-V2N101", "a55_cluster", "baremetal")
        self.assertEqual(rc, 0, msg=err)
        self.assertIn("ALP_SDK_USE_DRPAI=ON", out)
        self.assertNotIn("ALP_SDK_USE_DEEPX_DXM1=ON", out)

    # --- Schema-level rejection of inference.backend ------------------

    def test_inference_backend_field_rejected_by_schema(self) -> None:
        """board.yaml v2 cannot declare `inference.backend` -- silicon
        capability is not a project-level choice.  Schema's
        additionalProperties: false rejects the unknown property at
        validation time."""
        body = """
            schema_version: 2
            som:
              sku: E1M-AEN701
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
            schema_version: 2
            som:
              sku: E1M-AEN701
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


if __name__ == "__main__":
    unittest.main()
