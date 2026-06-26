/**
 * @file cap_instance.h
 * @brief Instance-level capability flags and struct.
 *
 * Distinct from the SoC-level ALP_CAP_* macros in soc_caps.h /
 * cap.h: those answer "does this silicon have an NPU at all?",
 * these answer "does THIS opened ADC instance support DMA?".
 *
 * Populated by each backend's ops->probe() at open time, cached
 * in the handle, returned by alp_<class>_capabilities().
 *
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.7 introduces the instance-cap surface alongside the
 *      backend-registry foundation.  Promoted to [ABI-STABLE]
 *      once at least three vendor families exercise it.
 */

#ifndef ALP_CAP_INSTANCE_H
#define ALP_CAP_INSTANCE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bitwise-OR'd flags describing what a single opened handle can do. */
typedef enum {
	ALP_INSTANCE_CAP_DMA           = 1u << 0, /**< Handle can drive transfers over DMA. */
	ALP_INSTANCE_CAP_HW_OVERSAMPLE = 1u << 1, /**< Hardware oversampling/decimation available. */
	ALP_INSTANCE_CAP_HW_TRIGGER    = 1u << 2, /**< Conversions can be hardware-triggered. */
	ALP_INSTANCE_CAP_DIFFERENTIAL  = 1u << 3, /**< Differential (vs single-ended) channels. */
} alp_instance_cap_t;

/** @brief Per-instance capability descriptor populated by ops->probe. */
typedef struct alp_capabilities {
	uint32_t flags;               /**< Bitwise-OR of @ref alp_instance_cap_t flags. */
	uint32_t max_sample_rate;     /**< Max sample rate, Hz; 0 = not applicable. */
	uint16_t max_resolution_bits; /**< Max sample resolution, bits; 0 = not applicable. */
	uint16_t channel_count;       /**< Number of channels on this instance. */
} alp_capabilities_t;

/**
 * @brief Test whether the descriptor advertises a capability flag.
 * @param c   Pointer returned by alp_<class>_capabilities().
 * @param f   A single flag from alp_instance_cap_t.
 * @return true if (c->flags & f) is non-zero; false otherwise.
 *         Returns false when c is NULL.
 */
bool alp_capabilities_has(const alp_capabilities_t *c, alp_instance_cap_t f);

#ifdef __cplusplus
}
#endif

#endif /* ALP_CAP_INSTANCE_H */
