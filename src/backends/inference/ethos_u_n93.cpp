/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * NXP i.MX 93 Ethos-U65 inference backend.  Registers against
 * silicon_ref="nxp:imx9:imx93" at priority 100 so it wins over
 * the portable tflm backend on i.MX 93 builds.
 *
 * The model-execution body is identical to the portable tflm
 * backend at source level -- AddEthosU() pulls in via
 * CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_N93=y inside
 * src/backends/inference/tflm.cpp, and the same vtable serves
 * both AEN and N93 builds (only the priority + silicon_ref +
 * vendor differ).  See src/backends/inference/ethos_u_aen.cpp
 * for the parallel AEN registration.
 *
 * What changes between AEN and N93 lives outside the SDK source:
 *
 *   - Vela target.  AEN uses --accelerator-config ethos-u55-256
 *     (or ethos-u85-256 on E4/E6/E8); N93 uses ethos-u65-256.
 *     Models compiled for the wrong target do not run.
 *   - Arm Ethos-U driver build.  AEN links U55-HP/U85-Tensor
 *     kernel sets; N93 links the U65-Dual kernel set.  The
 *     orchestrator-emitted CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U65
 *     selects which kernel set compiles in.
 *   - DT binding.  N93's NPU node lives in NXP's mcuxpresso-zephyr
 *     port at zephyr/boards/nxp/mimx9352_evk/.  The compatible
 *     property is "arm,ethos-u65"; alp_ethosu_n93_register attaches
 *     against the resolved IRQ + reg-base.
 *
 * Variant-specific attach hook lands when the NXP i.MX 93 BSP
 * port matures.  Until then alp_ethosu_n93_register returns
 * NOSUPPORT and the tflm backend below serves the same model
 * data via the Arm Ethos-U driver's default attach path on
 * Zephyr.
 */

#include <stddef.h>

extern "C" {
#include <alp/backend.h>
#include <alp/peripheral.h>
}

#include "tflm_shared.h"

extern "C" {

/**
 * @brief Attach the i.MX 93 Ethos-U65 NPU to the Arm Ethos-U driver.
 *
 * Called from the i.MX 93 BSP init pipeline once the NXP Zephyr
 * port lands the NPU DT node + IRQ wiring.  Today returns
 * NOSUPPORT so the symbol exists for BSP integration to link
 * against; the tflm backend body still serves Vela-compiled U65
 * models correctly via the Arm Ethos-U driver's default attach.
 *
 * @return ALP_OK on success, ALP_ERR_NOSUPPORT until the NXP
 *         BSP port lands the real attach sequence.
 */
alp_status_t alp_ethosu_n93_register(void)
{
	/* TBD: NXP mcuxpresso-zephyr port call sequence (ethosu_init
     * against the i.MX 93 U65 reg-base + IRQ pair).  The upstream
     * NPU node lands in NXP's port; today the DT compatible
     * = "arm,ethos-u65" wiring is incomplete. */
	return ALP_ERR_NOSUPPORT;
}

/* Compile-time sanity: building the N93 ethos-u backend without
 * the U65 variant selector means orchestrator drift -- U55 / U85
 * Vela models do not run on i.MX 93's U65 silicon. */
#if defined(CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_N93) &&                                       \
    !defined(CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U65)
#error "ETHOS_U_N93 backend enabled without VARIANT_U65; orchestrator drift"
#endif

const char *alp_ethosu_variant_name(void)
{
	return "ethos-u65";
}

ALP_BACKEND_REGISTER(inference,
                     ethos_u_n93,
                     {
                         /* .silicon_ref */ "nxp:imx9:imx93",
                         /* .vendor      */ "nxp",
                         /* .base_caps   */ 0u,
                         /* .priority    */ 100,
                         /* .ops         */ &alp_inference_tflm_ops,
                         /* .probe       */ NULL,
                     });

} /* extern "C" */
