/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_qenc dispatcher and per-backend
 * implementations.  NOT a public header.
 */

#ifndef ALP_BACKENDS_QENC_OPS_H
#define ALP_BACKENDS_QENC_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/counter.h>
#include <alp/peripheral.h>

typedef struct alp_qenc_ops alp_qenc_ops_t;

typedef struct alp_qenc_backend_state {
	void                 *dev; /* opaque backend device pointer
                                           * (const struct device * on Zephyr;
                                           * NULL for bridge; kept void* so the
                                           * portable handle does not pull in
                                           * <zephyr/device.h>) */
	uint32_t              encoder_id;
	int32_t               last_position; /* monotonic accumulator */
	void                 *be_data;
	const alp_qenc_ops_t *ops;
} alp_qenc_backend_state_t;

struct alp_qenc_ops {
	alp_status_t (*open)(const alp_qenc_config_t  *cfg,
	                     alp_qenc_backend_state_t *state,
	                     alp_capabilities_t       *caps_out);
	alp_status_t (*get_position)(alp_qenc_backend_state_t *state, int32_t *pos_out);
	alp_status_t (*reset_position)(alp_qenc_backend_state_t *state);
	void (*close)(alp_qenc_backend_state_t *state);
};

struct alp_qenc {
	alp_qenc_backend_state_t state;
	const alp_backend_t     *backend;
	alp_capabilities_t       cached_caps;
	bool                     in_use;
};

#endif /* ALP_BACKENDS_QENC_OPS_H */
