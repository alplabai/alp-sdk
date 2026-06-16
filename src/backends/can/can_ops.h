/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_can dispatcher and per-backend
 * implementations.  NOT a public header.
 */

#ifndef ALP_BACKENDS_CAN_OPS_H
#define ALP_BACKENDS_CAN_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/can.h>

typedef struct alp_can_ops alp_can_ops_t;

typedef struct alp_can_backend_state {
	void                *dev; /* opaque backend device pointer
                                          * (const struct device * on Zephyr;
                                          * kept void* so the portable handle
                                          * does not pull in <zephyr/device.h>) */
	uint32_t             bus_id;
	void                *be_data; /* per-handle backend sidecar */
	const alp_can_ops_t *ops;
} alp_can_backend_state_t;

struct alp_can_ops {
	alp_status_t (*open)(const alp_can_config_t *cfg, alp_can_backend_state_t *state,
	                     alp_capabilities_t *caps_out);
	alp_status_t (*start)(alp_can_backend_state_t *state);
	alp_status_t (*stop)(alp_can_backend_state_t *state);
	alp_status_t (*send)(alp_can_backend_state_t *state, const alp_can_frame_t *frame,
	                     uint32_t timeout_ms);
	alp_status_t (*add_filter)(alp_can_backend_state_t *state, const alp_can_filter_t *filter,
	                           alp_can_rx_cb_t cb, void *user, int32_t *filter_id_out);
	alp_status_t (*remove_filter)(alp_can_backend_state_t *state, int32_t filter_id);
	void (*close)(alp_can_backend_state_t *state);
};

/*
 * Portable handle layout.  Holds the caller's config snapshot + a
 * started flag so the dispatcher can short-circuit double-start/stop
 * without entering the backend and so close() can consult the flag
 * to decide whether to issue can_stop on shutdown.  Zephyr-specific
 * RX callback storage lives in a sidecar inside
 * src/backends/can/zephyr_drv.c so non-Zephyr backends never touch
 * a Zephyr type.
 */
struct alp_can {
	alp_can_backend_state_t state;
	const alp_backend_t    *backend;
	alp_capabilities_t      cached_caps;
	bool                    in_use;
	alp_can_config_t        cfg; /* snapshot of caller's config */
	bool                    started;
};

#endif /* ALP_BACKENDS_CAN_OPS_H */
