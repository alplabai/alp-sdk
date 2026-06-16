/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_dac dispatcher and per-backend
 * implementations.  NOT a public header -- customer code never
 * sees this struct.  Layout may change between SDK versions.
 */

#ifndef ALP_BACKENDS_DAC_OPS_H
#define ALP_BACKENDS_DAC_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/dac.h>
#include <alp/peripheral.h>

typedef struct alp_dac_ops alp_dac_ops_t;

/** Backend-owned per-handle state. */
typedef struct alp_dac_backend_state {
	void                *dev; /* opaque backend device pointer
                                       * (const struct device * on Zephyr;
                                       * kept void* so the portable handle
                                       * does not pull in <zephyr/device.h>) */
	uint32_t             channel_id;
	void                *be_data;
	const alp_dac_ops_t *ops;
} alp_dac_backend_state_t;

/** Vtable each backend implements. */
struct alp_dac_ops {
	/* Open the channel.  cfg is the customer's config; state is
     * preallocated by the dispatcher; caps_out is filled with the
     * (possibly probe-refined) instance capabilities.
     *
     * Returns ALP_OK on success; ALP_ERR_INVAL for a bad channel id;
     * ALP_ERR_NOT_READY if hardware isn't initialised; ALP_ERR_NOSUPPORT
     * when the backend has no DAC body (CONFIG_DAC=n).
     */
	alp_status_t (*open)(const alp_dac_config_t *cfg, alp_dac_backend_state_t *state,
	                     alp_capabilities_t *caps_out);

	/* Set the output in millivolts. */
	alp_status_t (*write_mv)(alp_dac_backend_state_t *state, uint16_t mv);

	/* Read back the programmed output in millivolts. */
	alp_status_t (*read_mv)(alp_dac_backend_state_t *state, uint16_t *mv_out);

	/* Tear down.  May be NULL for stateless backends. */
	void (*close)(alp_dac_backend_state_t *state);
};

/**
 * Handle struct layout.  Opaque to customers via the public
 * `typedef struct alp_dac alp_dac_t;`.  Defined here so both the
 * dispatcher (src/dac_dispatch.c) and the backend .c files can
 * reach the fields without duplicating the layout in each
 * translation unit.
 */
struct alp_dac {
	alp_dac_backend_state_t state;
	const alp_backend_t    *backend;
	alp_capabilities_t      cached_caps;
	bool                    in_use;
};

#endif /* ALP_BACKENDS_DAC_OPS_H */
