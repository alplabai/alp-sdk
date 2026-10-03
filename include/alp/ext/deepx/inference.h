/**
 * @file ext/deepx/inference.h
 * @brief DEEPX DX-M1 vendor-specific inference surface.
 *
 * Non-portable.  Include only when you've committed to DEEPX
 * silicon for the gated feature.  Every function in this header
 * verifies the handle's backend is DEEPX before touching
 * hardware; calls on a non-DEEPX handle return
 * @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC.
 *
 * Covers the two DX-M1 controls DEEPX's runtime (libdxrt) exposes
 * that the portable @ref alp_inference_config_t cannot express:
 * which of the DX-M1's three NPU cores a model runs on, and the
 * device's live temperature / clock / voltage telemetry.
 *
 * @note Linking `libdxrt` (which `libalp_sdk` does on V2M) installs
 *       SIGSEGV/SIGBUS/SIGABRT handlers that `exit(1)` -- see
 *       docs/soms/v2n-m1.md.
 *
 * The DX-M1 hangs off the Cortex-A55's PCIe and is driven by libdxrt
 * on Linux only, so on an M-class (Zephyr) build every call here
 * returns @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC after argument checks.
 *
 * @par Supported silicon: deepx:dx:m1
 *      DX-M1 today is shipped only on the V2N-M1 add-on; vendor
 *      packs may extend this list as DEEPX adds family members.
 *
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      Promotes to [ABI-STABLE] when three vendor families ship
 *      extensions.
 */

#ifndef ALP_EXT_DEEPX_INFERENCE_H
#define ALP_EXT_DEEPX_INFERENCE_H

#include <stdint.h>

#include <alp/inference.h>
#include <alp/peripheral.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Compile-time presence marker -- used by example code to gate vendor calls. */
#define ALP_EXT_DEEPX_INFERENCE_AVAILABLE 1

/** Number of NPU cores on a DX-M1. */
#define ALP_DEEPX_NPU_CORE_COUNT 3u

/** Which DX-M1 NPU cores a model runs on.  Values and order match
 *  libdxrt's `dxrt::InferenceOption::BOUND_OPTION`. */
typedef enum {
	ALP_DEEPX_NPU_CORES_ALL = 0u, /**< All three cores (libdxrt default). */
	ALP_DEEPX_NPU_CORE_0    = 1u, /**< Core 0 only. */
	ALP_DEEPX_NPU_CORE_1    = 2u, /**< Core 1 only. */
	ALP_DEEPX_NPU_CORE_2    = 3u, /**< Core 2 only. */
	ALP_DEEPX_NPU_CORES_01  = 4u, /**< Cores 0 and 1. */
	ALP_DEEPX_NPU_CORES_12  = 5u, /**< Cores 1 and 2. */
	ALP_DEEPX_NPU_CORES_02  = 6u, /**< Cores 0 and 2. */
} alp_deepx_npu_cores_t;

/** Live DX-M1 telemetry, one entry per NPU core. */
typedef struct {
	int32_t  temperature_c[ALP_DEEPX_NPU_CORE_COUNT];  /**< Degrees Celsius. */
	uint32_t npu_clock_mhz[ALP_DEEPX_NPU_CORE_COUNT];  /**< Core clock, MHz. */
	uint32_t npu_voltage_mv[ALP_DEEPX_NPU_CORE_COUNT]; /**< Core supply, mV. */
	uint64_t memory_bytes;                             /**< DX-M1 on-card DRAM size. */
} alp_deepx_device_status_t;

/**
 * @brief Open a DEEPX inference handle directly on a chosen set of NPU cores.
 *
 * @par Supported silicon: deepx:dx:m1
 *
 * Same as @ref alp_inference_open with the DEEPX backend, except the
 * libdxrt engine is built on @p cores from the start.  No temporary
 * all-cores engine is created, so no extra one of the DX-M1's three
 * driver queues is used (see the @warning on
 * @ref alp_deepx_inference_bind_cores for the 3-core-set limit).  Use it
 * to run two models side by side, e.g. one on
 * @ref ALP_DEEPX_NPU_CORES_01 and one on @ref ALP_DEEPX_NPU_CORE_2.
 * Release with @ref alp_inference_close like any other handle.
 *
 * @param[in] cfg    Model and options, as for @ref alp_inference_open.
 *                   @c backend must be @ref ALP_INFERENCE_BACKEND_AUTO
 *                   (treated as DEEPX here) or
 *                   @ref ALP_INFERENCE_BACKEND_DEEPX_DXM1;
 *                   @c format must be @ref ALP_INFERENCE_MODEL_DXNN.
 * @param[in] cores  Core set from @ref alp_deepx_npu_cores_t.
 *
 * @return  A handle, or NULL with the reason in @ref alp_last_error:
 *          @ref ALP_ERR_INVAL on NULL @p cfg, out-of-range @p cores, a
 *               non-DEEPX @c backend or a non-DXNN @c format.
 *          @ref ALP_ERR_BUSY if @p cores would be this process's fourth
 *               distinct core set.
 *          @ref ALP_ERR_NOMEM (also: handle pool full) /
 *               @ref ALP_ERR_IO / @ref ALP_ERR_NOSUPPORT as for
 *               @ref alp_inference_open.
 *          @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC when DEEPX is not built in
 *               (always, on a Zephyr build).
 */
alp_inference_t *alp_deepx_inference_open(const alp_inference_config_t *cfg,
                                          alp_deepx_npu_cores_t         cores);

/**
 * @brief Run this handle's model on a chosen set of DX-M1 NPU cores.
 *
 * @par Supported silicon: deepx:dx:m1
 *
 * libdxrt fixes the core binding when it builds the inference engine,
 * so this call rebuilds the engine for @p inf from the model bytes
 * passed to @ref alp_inference_open.  Useful to keep two models from
 * sharing cores, or to measure per-core throughput.
 *
 * @warning @c model_data from @ref alp_inference_open must still be
 *          valid.  Output tensors fetched before this call are invalid
 *          after it; fetch them again.  Input buffers stay valid.  Blocks
 *          until any in-flight invoke on @p inf finishes.
 *
 * @warning At most three DISTINCT core sets can be live on one DX-M1
 *          (driver DX_NORMAL_QUEUE_MAX = 3; same-set engines share a
 *          queue; @ref ALP_DEEPX_NPU_CORES_ALL is a set).  A fourth makes
 *          dx-rt abort the process or kill `dxrtd`.  This process refuses
 *          it with @ref ALP_ERR_BUSY; across processes nothing guards it.
 *          This call holds the old and new set while it swaps: prefer
 *          @ref alp_deepx_inference_open.  Two processes on dx-rt 3.2.0
 *          need the `dxrtd` service (#2398).
 *
 * @param[in] inf    Handle from @ref alp_inference_open opened
 *                   against DEEPX silicon.
 * @param[in] cores  Core set from @ref alp_deepx_npu_cores_t.
 *
 * @return  @ref ALP_OK on success.
 *          @ref ALP_ERR_INVAL on NULL handle or out-of-range @p cores.
 *          @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC if @p inf is not
 *               DEEPX-backed (always, on a Zephyr build).
 *          @ref ALP_ERR_NOT_READY if @p inf is not open.
 *          @ref ALP_ERR_BUSY if the new set would be this process's
 *               fourth distinct core set; @p inf keeps its binding.
 *          @ref ALP_ERR_IO / @ref ALP_ERR_NOMEM if libdxrt cannot
 *               build the new engine; @p inf keeps its previous
 *               binding and stays usable.
 */
alp_status_t alp_deepx_inference_bind_cores(alp_inference_t *inf, alp_deepx_npu_cores_t cores);

/**
 * @brief Read the DX-M1's live temperature, clock and voltage.
 *
 * @par Supported silicon: deepx:dx:m1
 *
 * Reads the device @p inf runs on through libdxrt's `DeviceStatus`.
 * Safe to call while another thread is invoking on @p inf.  Values are
 * libdxrt's own: on DX-M1 FW v2.4.0 NPU 0's voltage reads 2779096 mV,
 * the same figure `dxrt-cli -s` prints, so treat a value far outside the
 * 0.75 V rail as a firmware reading, not a fault.
 *
 * @param[in]  inf         Handle from @ref alp_inference_open
 *                         opened against DEEPX silicon.
 * @param[out] status_out  Receives the telemetry.  Must be non-NULL.
 *
 * @return  @ref ALP_OK on success.
 *          @ref ALP_ERR_INVAL on NULL @p inf or @p status_out.
 *          @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC if @p inf is not
 *               DEEPX-backed (always, on a Zephyr build).
 *          @ref ALP_ERR_NOT_READY if @p inf is not open.
 *          @ref ALP_ERR_IO if libdxrt cannot read the device.
 */
alp_status_t alp_deepx_inference_get_status(alp_inference_t           *inf,
                                            alp_deepx_device_status_t *status_out);

#ifdef __cplusplus
}
#endif

#endif /* ALP_EXT_DEEPX_INFERENCE_H */
