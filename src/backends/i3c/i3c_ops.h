/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_i3c dispatcher and per-backend
 * implementations.  NOT a public header -- customer code never
 * sees this struct.  Layout may change between SDK versions.
 */

#ifndef ALP_BACKENDS_I3C_OPS_H
#define ALP_BACKENDS_I3C_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/i3c.h>
#include <alp/peripheral.h>

typedef struct alp_i3c_ops alp_i3c_ops_t;

/** Backend-owned per-handle state. */
typedef struct alp_i3c_backend_state {
	void                *dev; /* opaque backend device pointer
	                           * (const struct device * on Zephyr;
	                           * kept void* so the portable handle
	                           * does not pull in <zephyr/device.h>) */
	uint32_t             bus_id;
	void                *be_data;
	const alp_i3c_ops_t *ops;
} alp_i3c_backend_state_t;

/** Vtable each backend implements. */
struct alp_i3c_ops {
	/* Open the bus.  cfg is the customer's config; state is
	 * preallocated by the dispatcher; caps_out is filled with the
	 * (possibly probe-refined) instance capabilities.
	 *
	 * Returns ALP_OK on success; ALP_ERR_INVAL for a bad bus id;
	 * ALP_ERR_NOT_READY if hardware isn't initialised; ALP_ERR_NOSUPPORT
	 * when the backend has no I3C body (CONFIG_I3C=n).
	 */
	alp_status_t (*open)(const alp_i3c_config_t  *cfg,
	                     alp_i3c_backend_state_t *state,
	                     alp_capabilities_t      *caps_out);

	/* Blocking private write to addr. */
	alp_status_t (*write)(alp_i3c_backend_state_t *state,
	                      uint8_t                  addr,
	                      const uint8_t           *data,
	                      size_t                   len);

	/* Blocking private read from addr. */
	alp_status_t (*read)(alp_i3c_backend_state_t *state, uint8_t addr, uint8_t *data, size_t len);

	/* Write-then-read (repeated START, no STOP in between). */
	alp_status_t (*write_read)(alp_i3c_backend_state_t *state,
	                           uint8_t                  addr,
	                           const uint8_t           *wdata,
	                           size_t                   wlen,
	                           uint8_t                 *rdata,
	                           size_t                   rlen);

	/* Tear down.  May be NULL for stateless backends. */
	void (*close)(alp_i3c_backend_state_t *state);
};

/**
 * Handle struct layout.  Opaque to customers via the public
 * `typedef struct alp_i3c alp_i3c_t;`.  Defined here so both the
 * dispatcher (src/i3c_dispatch.c) and the backend .c files can
 * reach the fields without duplicating the layout in each
 * translation unit.
 */
struct alp_i3c {
	alp_i3c_backend_state_t state;
	const alp_backend_t    *backend;
	alp_capabilities_t      cached_caps;
	/* lifecycle/active_ops drive the generic open/op/close guard in
	 * src/common/alp_slot_claim.h (alp_handle_op_enter/leave/
	 * begin_close, issue #629) -- placed before in_use so the atomic-
	 * claim zeroing in the dispatcher (memset up to
	 * offsetof(..., in_use)) resets both on every fresh claim. */
	uint8_t  lifecycle;
	uint32_t active_ops;
	bool     in_use;
};

#endif /* ALP_BACKENDS_I3C_OPS_H */
