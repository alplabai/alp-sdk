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
 * The NPU attach is NOT Alif-proprietary.  The "arm,ethos-u"
 * nodes (ethosu85@49042000 / ethosu55@400e1000 in the AEN dtsi,
 * reg-base + IRQ + TCM/SRAM global_base transcribed from the Alif
 * DFP) are bound by the UPSTREAM Arm Apache-2.0 ethos-u-core-driver
 * (the `hal_ethos_u` Zephyr module, drivers/misc/ethos_u/
 * ethos_u_arm.c), which runs ethosu_init() -- the NPU soft reset +
 * attach -- at POST_KERNEL with no vendor HAL pack.  The app
 * supplies the strong ethosu_address_remap()/ethosu_config_select()
 * weak-overrides (CPU->NPU translation + AXI-port routing); see the
 * aen-npu-inference* examples.  alp_ethosu_aen_register() below
 * therefore just confirms that auto-attach succeeded -- it is a
 * BSP-init readiness check, not a missing vendor sequence.  The
 * Vela target consistency (model U55 vs U85 vs build's compiled
 * kernel set) gets cross-checked at compile time via the
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

#if defined(CONFIG_ETHOS_U)
#include <zephyr/device.h>
#endif

extern "C" {

/**
 * @brief Confirm the AEN Ethos-U NPU attached to the Arm driver.
 *
 * Intended for the AEN BSP init pipeline (or a lazy check from the
 * first alp_inference_open()).  The Arm Apache-2.0 ethos-u-core-driver
 * (the `hal_ethos_u` Zephyr module) already binds every "arm,ethos-u"
 * node and runs ethosu_init() at POST_KERNEL using the reg-base / IRQ /
 * global_base carried in the AEN dtsi (DFP-sourced) -- no Alif vendor
 * HAL pack is involved.  This entry point just verifies that
 * auto-attach left at least one NPU device ready, so a caller can gate
 * NPU-dependent work on a real readiness signal.
 *
 * @return ALP_OK when an Ethos-U device is present and ready;
 *         ALP_ERR_NOT_PRESENT_ON_THIS_SOC when no "arm,ethos-u" node is
 *         enabled; ALP_ERR_NOSUPPORT when the build carries no Ethos-U
 *         core driver (e.g. native_sim) or the device is not ready.
 */
alp_status_t alp_ethosu_aen_register(void)
{
#if defined(CONFIG_ETHOS_U)
	/* DT_DRV_COMPAT of the upstream driver is `arm_ethos_u`; grab the
	 * first enabled instance (U85 or U55) the Arm driver attached. */
	const struct device *npu = DEVICE_DT_GET_ANY(arm_ethos_u);

	if (npu == NULL) {
		return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	}
	return device_is_ready(npu) ? ALP_OK : ALP_ERR_NOSUPPORT;
#else
	/* No Ethos-U core driver in this build -- the dispatcher falls
	 * back to the CPU tflm / sw_fallback backend. */
	return ALP_ERR_NOSUPPORT;
#endif
}

/* Compile-time sanity: building the AEN ethos-u backend without a
 * U-variant selector almost certainly means an orchestrator drift.
 * The cross-check protects HIL from a build that links the wrong
 * Vela kernels for the model. */
#if defined(CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_AEN) &&                                       \
    !defined(CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U55) &&                                      \
    !defined(CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U85)
#error "ETHOS_U_AEN backend enabled without VARIANT_U55 or _U85; orchestrator drift"
#endif

ALP_BACKEND_REGISTER(inference,
                     ethos_u_aen_e3,
                     {
                         /* .silicon_ref */ "alif:ensemble:e3",
                         /* .vendor      */ "alif",
                         /* .base_caps   */ 0u,
                         /* .priority    */ 100,
                         /* .ops         */ &alp_inference_tflm_ops,
                         /* .probe       */ NULL,
                     });

ALP_BACKEND_REGISTER(inference,
                     ethos_u_aen_e4,
                     {
                         /* .silicon_ref */ "alif:ensemble:e4",
                         /* .vendor      */ "alif",
                         /* .base_caps   */ 0u,
                         /* .priority    */ 100,
                         /* .ops         */ &alp_inference_tflm_ops,
                         /* .probe       */ NULL,
                     });

ALP_BACKEND_REGISTER(inference,
                     ethos_u_aen_e5,
                     {
                         /* .silicon_ref */ "alif:ensemble:e5",
                         /* .vendor      */ "alif",
                         /* .base_caps   */ 0u,
                         /* .priority    */ 100,
                         /* .ops         */ &alp_inference_tflm_ops,
                         /* .probe       */ NULL,
                     });

ALP_BACKEND_REGISTER(inference,
                     ethos_u_aen_e6,
                     {
                         /* .silicon_ref */ "alif:ensemble:e6",
                         /* .vendor      */ "alif",
                         /* .base_caps   */ 0u,
                         /* .priority    */ 100,
                         /* .ops         */ &alp_inference_tflm_ops,
                         /* .probe       */ NULL,
                     });

ALP_BACKEND_REGISTER(inference,
                     ethos_u_aen_e7,
                     {
                         /* .silicon_ref */ "alif:ensemble:e7",
                         /* .vendor      */ "alif",
                         /* .base_caps   */ 0u,
                         /* .priority    */ 100,
                         /* .ops         */ &alp_inference_tflm_ops,
                         /* .probe       */ NULL,
                     });

ALP_BACKEND_REGISTER(inference,
                     ethos_u_aen_e8,
                     {
                         /* .silicon_ref */ "alif:ensemble:e8",
                         /* .vendor      */ "alif",
                         /* .base_caps   */ 0u,
                         /* .priority    */ 100,
                         /* .ops         */ &alp_inference_tflm_ops,
                         /* .probe       */ NULL,
                     });

} /* extern "C" */
