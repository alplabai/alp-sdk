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
 * Covers the DRAM-tile + slot-management knobs the DX-M1
 * architecture exposes that the portable
 * @ref alp_inference_config_t cannot express.  The DX-M1 NPU
 * works in slots (independent inference contexts that share the
 * NPU's compute fabric but partition DRAM) -- TFLM-style
 * backends have no equivalent concept of compile-time-bound
 * DRAM tiles.
 *
 * @par Supported silicon: deepx:dx:m1
 *      DX-M1 today is shipped only on the V2N-M1 add-on; vendor
 *      packs may extend this list as DEEPX adds family members.
 *
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      Header lands ahead of the vendor pack body; every function
 *      returns @ref ALP_ERR_NOSUPPORT until the DEEPX SDK
 *      adapter lands in west.yml.  Promotes to [ABI-STABLE]
 *      when three vendor families ship extensions.
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

/** DX-M1 carries exactly four hardware slots.  Tile reservations
 *  are addressed by named-slot rather than bare 0..3 integers. */
typedef enum {
    ALP_DEEPX_INFERENCE_SLOT_0 = 0u,
    ALP_DEEPX_INFERENCE_SLOT_1 = 1u,
    ALP_DEEPX_INFERENCE_SLOT_2 = 2u,
    ALP_DEEPX_INFERENCE_SLOT_3 = 3u,
} alp_deepx_inference_slot_t;

#define ALP_DEEPX_INFERENCE_SLOT_COUNT 4u

/** DX-M1 runtime status flags. */
typedef enum {
    ALP_DEEPX_INFERENCE_STATUS_IDLE        = 0u,
    ALP_DEEPX_INFERENCE_STATUS_ARMED       = 1u << 0,
    ALP_DEEPX_INFERENCE_STATUS_RUNNING     = 1u << 1,
    ALP_DEEPX_INFERENCE_STATUS_DONE        = 1u << 2,
    ALP_DEEPX_INFERENCE_STATUS_TIMEOUT     = 1u << 3,
    ALP_DEEPX_INFERENCE_STATUS_TRANSPORT_ERR = 1u << 4,
} alp_deepx_inference_status_t;

/**
 * @brief Pin the inference to a specific hardware slot.
 *
 * @par Supported silicon: deepx:dx:m1
 *
 * DX-M1's NPU runs up to four inferences concurrently, one per
 * hardware slot.  By default the DEEPX runtime assigns the next
 * free slot; this call pins the handle to a specific slot
 * (useful when the customer wants deterministic DRAM placement
 * across runs, e.g. for benchmarking or for cooperating with a
 * sibling firmware that holds another slot).
 *
 * @param[in] inf   Handle from @ref alp_inference_open opened
 *                  against DEEPX silicon.
 * @param[in] slot  Target slot from @ref alp_deepx_inference_slot_t.
 *
 * @return  @ref ALP_OK on success.
 *          @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC if @p inf was
 *               opened on non-DEEPX silicon.
 *          @ref ALP_ERR_INVAL on NULL handle or out-of-range slot.
 *          @ref ALP_ERR_BUSY if the requested slot is held by
 *               another handle.
 *          @ref ALP_ERR_NOSUPPORT until the DEEPX SDK adapter
 *               lands.
 */
alp_status_t alp_deepx_inference_slot_pin(alp_inference_t *inf,
                                          alp_deepx_inference_slot_t slot);

/**
 * @brief Configure the DRAM tile reservation for this handle.
 *
 * @par Supported silicon: deepx:dx:m1
 *
 * DX-M1 lets each slot reserve a contiguous DRAM tile up front
 * so the runtime never has to migrate weights between invokes.
 * Setting this once before @ref alp_inference_invoke avoids the
 * first-invoke stall the default lazy-alloc path incurs.  The
 * exact tile granularity is silicon-specific (the V2N-M1 SoM
 * exposes 256 MB of dedicated DDR; tiles are powers of two
 * within that envelope).
 *
 * @param[in] inf          Handle from @ref alp_inference_open
 *                         opened against DEEPX silicon.
 * @param[in] tile_bytes   Target tile size in bytes.  Rounded
 *                         up to the next power-of-two by the
 *                         vendor pack body.
 *
 * @return  @ref ALP_OK / @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC /
 *          @ref ALP_ERR_INVAL (NULL inf) /
 *          @ref ALP_ERR_OUT_OF_RANGE (tile_bytes is zero or
 *               exceeds the SoM's DDR carve-out) /
 *          @ref ALP_ERR_NOSUPPORT until the DEEPX SDK adapter
 *               lands.
 */
alp_status_t alp_deepx_inference_dram_tile_reserve(alp_inference_t *inf,
                                                   uint32_t         tile_bytes);

/**
 * @brief Read the DX-M1 runtime status flags.
 *
 * @par Supported silicon: deepx:dx:m1
 *
 * Lets the application observe NPU state mid-invoke without
 * blocking on @ref alp_inference_invoke.  The returned bitmask
 * is the OR of @ref alp_deepx_inference_status_t flags.
 *
 * @param[in]  inf         Handle from @ref alp_inference_open
 *                         opened against DEEPX silicon.
 * @param[out] status_out  Receives the OR'd flag mask from
 *                         @ref alp_deepx_inference_status_t.
 *                         Must be non-NULL.
 *
 * @return  @ref ALP_OK / @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC /
 *          @ref ALP_ERR_INVAL (NULL inf or NULL status_out) /
 *          @ref ALP_ERR_NOSUPPORT until the DEEPX SDK adapter
 *               lands.
 */
alp_status_t alp_deepx_inference_get_status(alp_inference_t *inf,
                                            uint32_t        *status_out);

#ifdef __cplusplus
}
#endif

#endif /* ALP_EXT_DEEPX_INFERENCE_H */
