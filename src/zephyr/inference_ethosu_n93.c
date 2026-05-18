/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * NXP i.MX 93 Ethos-U65 per-variant config layer for the TFLM-backed
 * <alp/inference.h> executor.
 *
 * Why a separate file?  At source level the TFLM + Arm Ethos-U binding
 * in src/zephyr/inference_tflm.cpp is portable across U55 (Alif AEN's
 * NPU) and U65 (i.MX 93's NPU) -- both targets use the same
 * `AddEthosU()` op-resolver entry and the same `ethos_u_invoke` driver
 * path.  What changes per-variant is *outside* the SDK: the Vela
 * compiler's `--accelerator-config` flag (`ethos-u55-256` for AEN vs
 * `ethos-u65-256` for i.MX 93), the Arm Ethos-U driver build (U55-HP
 * vs U65-Dual configuration), and the SoC-side DT bindings for the
 * NPU IRQ / SRAM region.
 *
 * This file gives those variant-specific bits a fixed anchor point in
 * the build.  Compiled only when CONFIG_ALP_SDK_INFERENCE_ETHOS_U_N93=y
 * (the SDK's i.MX 93 + Ethos-U65 build switch); functionally a stub
 * today, with hooks that the v0.4 i.MX 93 bring-up fills in.
 *
 * Specifically v0.4 will:
 *   - Provide `alp_ethosu_n93_register` to attach the NPU device tree
 *     node + IRQ handler to the Arm Ethos-U driver at boot, mirroring
 *     the Alif Ensemble equivalent at vendors/alif/ (when that lands).
 *   - Optionally swap the TFLM op resolver's default 32-op cap for a
 *     larger one when U65's wider op set is in play.
 *   - Document the Vela invocation in
 *     `vendors/nxp-imx93/README.md` (already done in v0.3 scaffolding).
 *
 * Until v0.4 fills the bodies the symbol below is a no-op; the
 * dispatcher in inference_zephyr.c keeps dispatching `ETHOS_U` through
 * inference_tflm.cpp unchanged.  Apps that target Ethos-U65 work
 * through the same `<alp/inference.h>` surface as on AEN.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/inference.h"

/**
 * @brief Register the i.MX 93 Ethos-U65 NPU with the Arm Ethos-U driver.
 *
 * Called from `inference_tflm_open` when CONFIG_ALP_SDK_INFERENCE_ETHOS_U_N93=y
 * is active in the build.  v0.4 fills the body once the i.MX 93 BSP
 * port lands and the Cortex-M33 boots Zephyr with the NPU node
 * decorated by a `compatible = "arm,ethos-u65"` DT entry.
 *
 * @return ALP_OK on success, ALP_ERR_NOSUPPORT until v0.4 lands the
 *         real attach sequence.
 */
alp_status_t alp_ethosu_n93_register(void)
{
    /* v0.4 calls ethosu_init() (Arm driver) against the DT-resolved
     * regbase + IRQ pair; the upstream NPU node lands in NXP's
     * mcuxpresso-zephyr port. */
    return ALP_ERR_NOSUPPORT;
}

/**
 * @brief Report the active Ethos-U variant for downstream code (e.g.
 *        op-resolver sizing or Vela-target sanity-check helpers).
 *
 * @return A short literal: "ethos-u65" on this build.  Mirrors what
 *         Vela's `--accelerator-config` flag was set to when the
 *         model in `cfg.model_data` was compiled; mismatch is a
 *         user-side error today (no runtime cross-check yet).
 *
 * @note   Cross-checked at compile time against the per-variant
 *         CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U65 switch emitted by the
 *         orchestrator (G-1 wiring).  If a future build flips this
 *         file on without setting U65 the assert fires immediately,
 *         catching Kconfig drift before it reaches HIL.
 */
#if defined(CONFIG_ALP_SDK_INFERENCE_ETHOS_U_N93) && \
    !defined(CONFIG_ALP_SDK_INFERENCE_ETHOS_U_U65)
#error "ALP_SDK_INFERENCE_ETHOS_U_N93 selected without ALP_SDK_INFERENCE_ETHOS_U_U65; orchestrator drift"
#endif

const char *alp_ethosu_variant_name(void)
{
    return "ethos-u65";
}
