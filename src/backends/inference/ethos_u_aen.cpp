/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif Ensemble Ethos-U inference backend.  Registers against the
 * AEN family silicon_refs at priority 100 so it wins over the
 * portable tflm (priority 50) on Ensemble SKUs that carry a U55
 * pair (E3 / E5 / E7) or a U55 pair + U85 (E4 / E6 / E8).
 *
 * The actual model-execution body is identical to the portable
 * tflm backend at source level (TFLM + AddEthosU() op resolver
 * entry registers the Ethos-U custom op so Vela-compiled .tflite
 * files dispatch their NPU offload).  This file reuses
 * alp_inference_tflm_ops verbatim and only differs in the
 * ALP_BACKEND_REGISTER row.
 *
 * What does change between AEN and i.MX 93 lives outside the
 * SDK source: the Vela compiler's --accelerator-config flag
 * (ethos-u55-256 / ethos-u85-256 on AEN; ethos-u65-256 on
 * i.MX 93), the Arm Ethos-U driver library build (U55-HP vs
 * U65-Dual vs U85-Tensor configurations), and the SoC-side DT
 * binding for the NPU IRQ / SRAM region.  The orchestrator
 * (scripts/alp_orchestrate.py) emits the matching CONFIG_ trio
 * (ALP_SDK_INFERENCE_BACKEND_ETHOS_U_AEN +
 *  ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U55/U85 +
 *  ALP_SDK_INFERENCE_TFLM_KERNEL_HELIUM) for AEN slices from
 * the SoM preset's inference.npu_population[] + the SoC JSON's
 * cores[<id>].vector_extension.
 *
 * Variant-specific attach hook (Alif HAL ethosu_init) lands
 * alongside the Alif Ethos-U vendor pack; the entry point is
 * alp_ethosu_aen_register() declared here -- today returns
 * NOSUPPORT until the pack drops.  The Vela target consistency
 * (model U55 vs U85 vs build's compiled kernel set) gets
 * cross-checked at compile time via the
 * INFERENCE_ETHOS_U_VARIANT_* Kconfigs.
 *
 * Native-sim builds skip CONFIG_TENSORFLOW_LITE_MICRO so this
 * TU is excluded from that build; the dispatcher falls back to
 * sw_fallback (priority 0, returns NOSUPPORT).
 */

#include <stddef.h>

extern "C" {
#include <alp/backend.h>
#include <alp/peripheral.h>
}

#include "tflm_shared.h"

extern "C" {

/**
 * @brief Attach the AEN Ethos-U NPU to the Arm Ethos-U driver.
 *
 * Called from the AEN BSP init pipeline (or lazily from the first
 * alp_inference_open() call) once the Alif Ethos-U HAL vendor pack
 * lands.  Today the body returns NOSUPPORT so the symbol exists
 * for the BSP integration to link against without functional
 * effect; the tflm backend below still serves CPU + AddEthosU()
 * builds correctly via the Arm driver's default attach on Zephyr.
 *
 * @return ALP_OK on success, ALP_ERR_NOSUPPORT until the Alif
 *         HAL pack lands the real attach sequence.
 */
alp_status_t alp_ethosu_aen_register(void)
{
    /* TBD: Alif Ethos-U HAL pack call sequence (ethosu_init
     * against the AEN U55/U85 reg-base + IRQ).  The Arm Ethos-U
     * driver Zephyr port can also register without an explicit
     * attach when the DT compatible = "arm,ethos-u" node carries
     * the AEN-specific properties. */
    return ALP_ERR_NOSUPPORT;
}

/* Compile-time sanity: building the AEN ethos-u backend without a
 * U-variant selector almost certainly means an orchestrator drift.
 * The cross-check protects HIL from a build that links the wrong
 * Vela kernels for the model. */
#if defined(CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_AEN) && \
    !defined(CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U55) && \
    !defined(CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U85)
#error "ETHOS_U_AEN backend enabled without VARIANT_U55 or _U85; orchestrator drift"
#endif

ALP_BACKEND_REGISTER(inference, ethos_u_aen_e3, {
    /* .silicon_ref */ "alif:ensemble:e3",
    /* .vendor      */ "alif",
    /* .base_caps   */ 0u,
    /* .priority    */ 100,
    /* .ops         */ &alp_inference_tflm_ops,
    /* .probe       */ NULL,
});

ALP_BACKEND_REGISTER(inference, ethos_u_aen_e4, {
    /* .silicon_ref */ "alif:ensemble:e4",
    /* .vendor      */ "alif",
    /* .base_caps   */ 0u,
    /* .priority    */ 100,
    /* .ops         */ &alp_inference_tflm_ops,
    /* .probe       */ NULL,
});

ALP_BACKEND_REGISTER(inference, ethos_u_aen_e5, {
    /* .silicon_ref */ "alif:ensemble:e5",
    /* .vendor      */ "alif",
    /* .base_caps   */ 0u,
    /* .priority    */ 100,
    /* .ops         */ &alp_inference_tflm_ops,
    /* .probe       */ NULL,
});

ALP_BACKEND_REGISTER(inference, ethos_u_aen_e6, {
    /* .silicon_ref */ "alif:ensemble:e6",
    /* .vendor      */ "alif",
    /* .base_caps   */ 0u,
    /* .priority    */ 100,
    /* .ops         */ &alp_inference_tflm_ops,
    /* .probe       */ NULL,
});

ALP_BACKEND_REGISTER(inference, ethos_u_aen_e7, {
    /* .silicon_ref */ "alif:ensemble:e7",
    /* .vendor      */ "alif",
    /* .base_caps   */ 0u,
    /* .priority    */ 100,
    /* .ops         */ &alp_inference_tflm_ops,
    /* .probe       */ NULL,
});

ALP_BACKEND_REGISTER(inference, ethos_u_aen_e8, {
    /* .silicon_ref */ "alif:ensemble:e8",
    /* .vendor      */ "alif",
    /* .base_caps   */ 0u,
    /* .priority    */ 100,
    /* .ops         */ &alp_inference_tflm_ops,
    /* .probe       */ NULL,
});

}  /* extern "C" */
