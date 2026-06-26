/**
 * @file ext/renesas/inference.h
 * @brief Renesas DRP-AI3 vendor-specific inference surface.
 *
 * Non-portable.  Include only when you've committed to Renesas
 * RZ/V2N silicon for the gated feature.  Every function in
 * this header verifies the handle's backend is Renesas before
 * touching hardware; calls on a non-Renesas handle return
 * @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC.
 *
 * Covers the DRP-AI3 pipeline-stage configuration that the
 * portable @ref alp_inference_config_t cannot express: DRP-AI
 * splits an inference into a chain of stages (DRP, AI-MAC,
 * AI-SRAM, DMA) where the customer can pin which stage runs on
 * which sub-block and what the staging buffer reservation looks
 * like.  This is silicon-architecturally specific to Renesas's
 * dataflow NPU -- TFLM-style backends do not have an equivalent
 * notion.
 *
 * @par Supported silicon: renesas:rzv2n:n44
 *      DRP-AI3 today is only shipped on RZ/V2N N44; vendor
 *      packs may extend this list when V2M / V2L variants gain
 *      DRP-AI3 silicon.
 *
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      Header lands ahead of the vendor pack body; every function
 *      returns @ref ALP_ERR_NOSUPPORT until Renesas DRP-AI HAL
 *      integration lands.  Promotes to [ABI-STABLE] when three
 *      vendor families ship extensions.
 */

#ifndef ALP_EXT_RENESAS_INFERENCE_H
#define ALP_EXT_RENESAS_INFERENCE_H

#include <stdint.h>

#include <alp/inference.h>
#include <alp/peripheral.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Compile-time presence marker -- used by example code to gate vendor calls. */
#define ALP_EXT_RENESAS_INFERENCE_AVAILABLE 1

/** DRP-AI pipeline-stage selector.  Each named entry maps to one
 *  of the four execution units inside DRP-AI3; the customer can
 *  pin a model layer to a specific stage when the translator
 *  output leaves the choice open. */
typedef enum {
	ALP_RENESAS_INFERENCE_STAGE_DRP     = 0u, /**< DRP fabric (data-reconfigurable processor). */
	ALP_RENESAS_INFERENCE_STAGE_AI_MAC  = 1u, /**< AI-MAC unit (multiply-accumulate array). */
	ALP_RENESAS_INFERENCE_STAGE_AI_SRAM = 2u, /**< AI-SRAM staging buffer. */
	ALP_RENESAS_INFERENCE_STAGE_DMA     = 3u, /**< DMA controller (off-chip transfer). */
} alp_renesas_inference_stage_t;

/** DRP-AI runtime status flags. */
typedef enum {
	ALP_RENESAS_INFERENCE_STATUS_IDLE    = 0u,      /**< No inference armed or running. */
	ALP_RENESAS_INFERENCE_STATUS_ARMED   = 1u << 0, /**< Descriptors loaded, awaiting start. */
	ALP_RENESAS_INFERENCE_STATUS_RUNNING = 1u << 1, /**< NPU actively executing the model. */
	ALP_RENESAS_INFERENCE_STATUS_DONE    = 1u << 2, /**< Last invoke completed successfully. */
	ALP_RENESAS_INFERENCE_STATUS_TIMEOUT = 1u << 3, /**< Hardware watchdog fired before done. */
	ALP_RENESAS_INFERENCE_STATUS_BUS_ERR = 1u << 4, /**< Bus/transfer error during execution. */
} alp_renesas_inference_status_t;

/**
 * @brief Pin a model layer to a specific DRP-AI pipeline stage.
 *
 * @par Supported silicon: renesas:rzv2n:n44
 *
 * The DRP-AI translator output normally lets the runtime pick
 * the stage per layer based on the operator family (CONV/POOL
 * on DRP, MAC-heavy on AI-MAC, etc.).  This call overrides the
 * default for the named @p layer_index slot -- useful when the
 * customer profiled the model and found a different placement
 * beats the default by enough margin to matter.
 *
 * @param[in] inf          Handle from @ref alp_inference_open
 *                         opened against Renesas silicon.
 * @param[in] layer_index  Zero-based index into the translator's
 *                         layer list.  Out-of-range returns
 *                         @ref ALP_ERR_OUT_OF_RANGE.
 * @param[in] stage        Target stage from
 *                         @ref alp_renesas_inference_stage_t.
 *
 * @return  @ref ALP_OK on success.
 *          @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC if @p inf was
 *               opened on non-Renesas silicon.
 *          @ref ALP_ERR_INVAL on NULL handle or unknown stage.
 *          @ref ALP_ERR_OUT_OF_RANGE if layer_index exceeds the
 *               translator's layer count.
 *          @ref ALP_ERR_NOSUPPORT until the vendor pack body lands.
 */
alp_status_t alp_renesas_inference_pipeline_stage_pin(alp_inference_t              *inf,
                                                      uint32_t                      layer_index,
                                                      alp_renesas_inference_stage_t stage);

/**
 * @brief Configure the AI-SRAM staging buffer reservation.
 *
 * @par Supported silicon: renesas:rzv2n:n44
 *
 * DRP-AI3 carries a dedicated AI-SRAM that the runtime uses as
 * a staging buffer between DRP and AI-MAC stages.  The
 * reservation size determines how much of that SRAM the
 * inference is allowed to consume; oversized models spill to
 * external DDR (slower).  This call lets the customer pin the
 * reservation explicitly when the translator's auto-sizing
 * picks a worse trade-off than they want.
 *
 * @param[in] inf            Handle from @ref alp_inference_open
 *                           opened against Renesas silicon.
 * @param[in] reserve_bytes  Target reservation in bytes.  Clamped
 *                           to the AI-SRAM physical size on the
 *                           N44 (1.5 MB) when too large.
 *
 * @return  @ref ALP_OK / @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC /
 *          @ref ALP_ERR_INVAL (NULL inf) /
 *          @ref ALP_ERR_OUT_OF_RANGE (reserve_bytes is zero or
 *               exceeds the physical AI-SRAM size) /
 *          @ref ALP_ERR_NOSUPPORT until the vendor pack body lands.
 */
alp_status_t alp_renesas_inference_ai_sram_reserve(alp_inference_t *inf, uint32_t reserve_bytes);

/**
 * @brief Read the DRP-AI runtime status flags.
 *
 * @par Supported silicon: renesas:rzv2n:n44
 *
 * Lets the application observe NPU state mid-invoke (e.g. from
 * a watchdog thread) without blocking on
 * @ref alp_inference_invoke.  The returned bitmask is the OR of
 * @ref alp_renesas_inference_status_t flags.
 *
 * @param[in]  inf         Handle from @ref alp_inference_open
 *                         opened against Renesas silicon.
 * @param[out] status_out  Receives the OR'd flag mask from
 *                         @ref alp_renesas_inference_status_t.
 *                         Must be non-NULL.
 *
 * @return  @ref ALP_OK / @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC /
 *          @ref ALP_ERR_INVAL (NULL inf or NULL status_out) /
 *          @ref ALP_ERR_NOSUPPORT until the vendor pack body lands.
 */
alp_status_t alp_renesas_inference_get_status(alp_inference_t *inf, uint32_t *status_out);

#ifdef __cplusplus
}
#endif

#endif /* ALP_EXT_RENESAS_INFERENCE_H */
