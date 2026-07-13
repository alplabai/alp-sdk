# SPDX-License-Identifier: Apache-2.0
"""
Unit tests for scripts/alp_project.py -- §D.lib cross-library
HW-backend wiring (TestHwBackendsLoader) and silicon-determined
inference-backend / TFLM-kernel emission from the SoM preset's
capabilities matrix (TestInferenceFromSomCaps).

Run locally:

    python -m unittest tests.scripts.test_project_backends

Or via CI as configured in .github/workflows/pr-metadata-validate.yml.
"""

from __future__ import annotations

import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from _project_support import LOADER, _run_loader, _write_board  # noqa: E402


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
        "gfx_compat", "minimp3", "opus",
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
        """V2N101: no Ethos, primary NPU is DRP-AI; TLS-library CAU
        entries remain planned until a real library consumer lands."""
        self.assertEmitted    ("E1M-V2N101", "CONFIG_ALP_TFLM_DRP_AI=y")
        self.assertNotEmitted ("E1M-V2N101", "CONFIG_ALP_TFLM_ETHOS_U55=y")
        self.assertNotEmitted ("E1M-V2N101", "CONFIG_ALP_TFLM_ETHOS_U85=y")
        self.assertEmitted    ("E1M-V2N101", "CONFIG_ALP_TFLM_NEON=y")
        self.assertNotEmitted ("E1M-V2N101", "CONFIG_ALP_MBEDTLS_CAU=y")
        self.assertNotEmitted ("E1M-V2N101", "CONFIG_ALP_BEARSSL_CAU=y")
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
        """OPTIGA Trust M is populated on AEN + V2N + NX9101, but the
        mbedTLS/BearSSL handshake integration is still planned.  The
        loader must not emit active TLS offload claims for it."""
        self.assertNotEmitted ("E1M-AEN401", "CONFIG_ALP_MBEDTLS_CRYPTOCELL=y")
        self.assertNotEmitted ("E1M-AEN401", "CONFIG_ALP_MBEDTLS_OPTIGA=y")
        self.assertNotEmitted ("E1M-NX9101", "CONFIG_ALP_MBEDTLS_OPTIGA=y")
        self.assertNotEmitted ("E1M-NX9101", "CONFIG_ALP_MBEDTLS_CRYPTOCELL=y")

    def test_sw_fallback_always_emitted(self) -> None:
        """Each library's SW-fallback CONFIG_*=y is emitted
        unconditionally from its manifest's sw_fallback knob (separate
        from the hw-backends loader).  Both new §D.lib libraries and the 4
        baseline ones (lvgl / mbedtls / cmsis_dsp / littlefs) emit
        their fallback knob alongside the upstream library knob."""
        out = self._emit("E1M-AEN401")
        for fallback in (
            # §D.lib new libraries
            "CONFIG_ALP_TFLM_REF_KERNELS=y",
            "CONFIG_ALP_BEARSSL_PURE_C=y",
            "CONFIG_ALP_OPUS_PURE_C=y",
            "CONFIG_ALP_MINIMP3_PURE_C=y",
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


if __name__ == "__main__":
    unittest.main()
