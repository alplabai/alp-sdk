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
 * Two-word design: a universal `flags` word plus a class-scoped
 * `class_flags` word, so no single bit ever means two different
 * things depending which class handle you happen to be holding.
 *
 * @par flags (alp_instance_cap_t) -- universal, meaning fixed here:
 *      ALP_INSTANCE_CAP_REPORTED marks that the backend deliberately
 *      populated this descriptor.  `flags == 0` means "not reported"
 *      -- the backend never spoke -- never "has nothing".  Once
 *      REPORTED is set, every other bit (in `flags` or `class_flags`)
 *      that is clear is an affirmative "does not have it", not an
 *      unknown.  Likewise `channel_count == 0` alongside REPORTED
 *      means "serves none", distinct from "not reported".
 *
 * @par class_flags -- meaning owned by the class header of the
 *      handle you queried (e.g. ALP_ADC_CAP_* in <alp/adc.h>).  Two
 *      different classes may reuse the same bit position for
 *      unrelated facts; class_flags is only ever read next to a
 *      handle whose class you already know.
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

/**
 * @brief Bitwise-OR'd universal flags describing a single opened handle.
 *
 * These bits mean the same thing for every peripheral class.
 * Class-specific facts (e.g. ADC oversample/trigger/differential)
 * live in the class header's own `ALP_<CLASS>_CAP_*` constants and
 * are carried in `alp_capabilities_t.class_flags`, not here.
 */
typedef enum {
	/** Backend deliberately populated this descriptor.  `flags == 0`
	 *  means "not reported" -- the backend never spoke -- never "has
	 *  nothing".  See the file-level doc for the full contract. */
	ALP_INSTANCE_CAP_REPORTED = 1u << 0,
	/** The instance is backed by a DMA engine. */
	ALP_INSTANCE_CAP_DMA = 1u << 1,
} alp_instance_cap_t;

/** Per-instance capability descriptor populated by ops->probe. */
typedef struct alp_capabilities {
	uint32_t flags;       /* alp_instance_cap_t bits (universal) */
	uint32_t class_flags; /* ALP_<CLASS>_CAP_* bits; meaning owned by the
	                        * class header of the handle you queried */
	uint32_t max_rate_hz; /* class-defined rate ceiling: ADC samples/s,
	                        * I2C/SPI bus clock, PWM carrier. 0 = not reported */
	uint16_t max_resolution_bits;
	uint16_t channel_count;
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
