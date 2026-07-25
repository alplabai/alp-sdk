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
 * attach -- at POST_KERNEL with no vendor HAL pack.  This backend
 * supplies the strong ethosu_address_remap()/ethosu_config_select()
 * weak-overrides (CPU->NPU translation + AXI-port routing) below, so
 * an app no longer has to.  alp_ethosu_aen_register() below
 * therefore just confirms that auto-attach succeeded -- it is a
 * BSP-init readiness check, not a missing vendor sequence.  The
 * Vela target consistency (model U55 vs U85 vs build's compiled
 * kernel set) gets cross-checked at compile time via the
 * INFERENCE_ETHOS_U_VARIANT_* Kconfigs.
 *
 * This TU compiles whenever CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_AEN
 * is set (an AEN-family emit with the tflite-micro module present) --
 * including under native_sim on an alp-sdk-topdir workspace, where the
 * Ethos-U op registers against the module's stub Register_ETHOSU().  A
 * bare upstream-`zephyr` workspace without the module degrades
 * TENSORFLOW_LITE_MICRO to n, so BACKEND_TFLM (and this backend) drop out
 * and the dispatcher falls back to sw_fallback (priority 0, NOSUPPORT).
 * Real Ethos-U acceleration needs CONFIG_ETHOS_U + the hal_ethos_u driver
 * on AEN silicon.
 */

#include <stddef.h>

extern "C" {
#include <alp/backend.h>
#include <alp/peripheral.h>
}

#include "tflm_shared.h"

#if defined(CONFIG_ETHOS_U)
#include <zephyr/device.h>
/* hal_alif local_to_global(): CPU-local (ITCM/DTCM) -> NPU global AXI view. */
#include <soc_memory_map.h>
#endif

extern "C" {

#if defined(CONFIG_ETHOS_U)
/*
 * STRONG ethosu_address_remap -- the matched-runtime fix for ethosu_invoke=1.
 *
 * The Arm Ethos-U core driver programs the NPU's QBASE + every BASEP via the
 * WEAK ethosu_address_remap(addr, index) (ethosu_device_u85.c), whose default
 * returns the address UNCHANGED -- the CPU-local M55 view -- which for any
 * ITCM/DTCM-resident buffer is NOT the address the NPU's AXI master must use,
 * so the NPU reads the wrong memory and ethosu_invoke fails (status 1).  This
 * strong override translates to the NPU global view via hal_alif's own
 * local_to_global() (soc_memory_map.h) -- nothing re-authored.  For an
 * SRAM0-resident model/arena it is a no-op (SRAM0 @0x02000000 is already
 * global); the override is what makes the general path correct.  Previously
 * every AEN NPU app had to supply this itself; the SDK backend now owns it.
 */
uint64_t ethosu_address_remap(uint64_t address, int index)
{
	(void)index;
	/* Double cast silences the pointer/integer width warning on 32-bit M55. */
	return local_to_global((void *)(uint32_t)address);
}

/*
 * STRONG ethosu_config_select -- the SECOND half of the invoke=1 fix (NPU AXI
 * PORT selection), bench-isolated on E8.
 *
 * The core driver programs QCONFIG (command stream) + REGIONCFG (each BASEP
 * region) via the WEAK ethosu_config_select(addr, index), returning a MEM_ATTR
 * index whose bit2 picks the AXI master PORT: 0/1 -> SRAM port, 2/3 -> EXT
 * port.  The Arm defaults route the command stream (NPU_QCONFIG=2) + region 0
 * to the EXT port -- correct for the Arm reference layout (weights in external
 * flash/DRAM), but WRONG for Alif's `Ethos_U85_SRAM_Only` Vela system-config
 * where model + command stream + arena ALL live in SRAM0 (reachable only over
 * the SRAM port).  Sending the SRAM-resident command stream over EXT bus-aborts
 * on the first fetch: BENCH-CONFIRMED on E8 (ETHOS_U_LOG_LEVEL=DBG) invoke
 * returns 1 with NPU STATUS=0x00000804 (bus_status=1, faulting_interface=1).
 * Pinning every access to a SRAM-port MEM_ATTR (index 0) clears the fault.
 * MEM_ATTR index 0 (bit2=0) = SRAM AXI port, for the command stream (index -1)
 * and all regions -- everything a SRAM_Only model touches is SRAM0-resident.
 */
unsigned int ethosu_config_select(uint64_t address, int index)
{
	(void)address;
	(void)index;
	return 0U;
}
#endif /* CONFIG_ETHOS_U */

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
#if defined(CONFIG_ALP_SDK_INFERENCE_BACKEND_ETHOS_U_AEN) && \
    !defined(CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U55) && \
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
