/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_adc dispatcher and per-backend
 * implementations.  NOT a public header -- customer code never
 * sees this struct.  Layout may change between SDK versions.
 *
 * Vendor-specific backends extend alp_adc_ops_t by embedding it
 * as the first member of a larger struct (C first-member-aliasing
 * pattern).  Vendor functions cast the const void * ops field of
 * alp_backend_t back to the larger struct after verifying the
 * vendor name on the handle's backend.
 */

#ifndef ALP_BACKENDS_ADC_OPS_H
#define ALP_BACKENDS_ADC_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include <alp/adc.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

typedef struct alp_adc_ops alp_adc_ops_t;

/** Backend-owned per-handle state. */
typedef struct alp_adc_backend_state {
	uint32_t             reference_uv;
	uint16_t             resolution_bits;
	const alp_adc_ops_t *ops;
	void                *be_data;
} alp_adc_backend_state_t;

/** Vtable each backend implements. */
struct alp_adc_ops {
	/* Open the channel.  cfg is the customer's config; state is
     * preallocated by the dispatcher; caps_out is filled with the
     * (possibly probe-refined) instance capabilities.
     *
     * The backend MUST set state->reference_uv and
     * state->resolution_bits before returning ALP_OK.
     *
     * Returns ALP_OK on success; ALP_ERR_NOSUPPORT for bad cfg
     * (e.g. resolution_bits exceeds the SoC's max); ALP_ERR_NOT_READY
     * if hardware isn't initialised; ALP_ERR_NOMEM if the backend's
     * per-instance state pool is exhausted.
     */
	alp_status_t (*open)(const alp_adc_config_t  *cfg,
	                     alp_adc_backend_state_t *state,
	                     alp_capabilities_t      *caps_out);

	/* One-shot raw read.  Signed for symmetry with differential
     * mode; single-ended SoCs return non-negative values. */
	alp_status_t (*read_raw)(alp_adc_backend_state_t *state, int32_t *raw_out);

	/* Tear down.  May be NULL for stateless backends. */
	void (*close)(alp_adc_backend_state_t *state);
};

/**
 * Handle struct layout.  Opaque to customers via the public
 * `typedef struct alp_adc alp_adc_t;`.  Defined here so both the
 * dispatcher (src/adc_dispatch.c) and vendor-ext backends
 * (src/backends/adc/alif_e7.c, etc.) can access the fields without
 * duplicating the layout in each translation unit.
 */
struct alp_adc {
	alp_adc_backend_state_t state;
	const alp_backend_t    *backend;
	alp_capabilities_t      cached_caps;
	bool                    in_use;
};

#endif /* ALP_BACKENDS_ADC_OPS_H */
