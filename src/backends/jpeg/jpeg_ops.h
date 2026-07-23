/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_jpeg dispatcher and per-backend
 * implementations.  NOT a public header -- customer code never
 * sees this struct.  Layout may change between SDK versions.
 *
 * Task 1 ships only the zephyr_stub backend (every op returns
 * ALP_ERR_NOT_IMPLEMENTED); the software baseline + Alif Hantro
 * VC9000E backends land in later tasks per the tracking plan.
 */

#ifndef ALP_BACKENDS_JPEG_OPS_H
#define ALP_BACKENDS_JPEG_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/jpeg.h>

typedef struct alp_jpeg_ops alp_jpeg_ops_t;

/** Backend-owned per-handle state. */
typedef struct alp_jpeg_backend_state {
	void                 *be_data;
	const alp_jpeg_ops_t *ops;
} alp_jpeg_backend_state_t;

/** Vtable each jpeg backend implements. */
struct alp_jpeg_ops {
	alp_status_t (*open)(const alp_jpeg_config_t  *cfg,
	                     alp_jpeg_backend_state_t *state,
	                     alp_jpeg_caps_t          *caps_out);
	alp_status_t (*encode)(alp_jpeg_backend_state_t    *state,
	                       const alp_jpeg_encode_req_t *req,
	                       void                        *out_buf,
	                       size_t                       out_cap,
	                       size_t                      *out_len);
	void (*close)(alp_jpeg_backend_state_t *state);
};

/**
 * Handle struct layout.  Opaque to customers via the public
 * `typedef struct alp_jpeg alp_jpeg_t;` forward declaration in
 * <alp/jpeg.h>.  Defined here so the dispatcher
 * (src/jpeg_dispatch.c) and any future per-backend .c files can
 * access the fields without duplicating the layout.
 */
struct alp_jpeg {
	alp_jpeg_backend_state_t state;
	const alp_backend_t     *backend;
	alp_jpeg_caps_t          cached_caps;
	/* lifecycle/active_ops drive the generic open/op/close guard in
	 * src/common/alp_slot_claim.h (alp_handle_op_enter/leave/
	 * begin_close) -- placed before in_use so the atomic-claim
	 * zeroing in the dispatcher (memset up to offsetof(..., in_use))
	 * resets both on every fresh claim. */
	uint8_t  lifecycle;
	uint32_t active_ops;
	bool     in_use; /* MUST be the last member (slot-claim zeroing). */
};

#endif /* ALP_BACKENDS_JPEG_OPS_H */
