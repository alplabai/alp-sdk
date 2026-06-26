/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_display dispatcher and per-backend
 * implementations.  NOT a public header -- customer code never
 * sees this struct.  Layout may change between SDK versions.
 *
 * Slice 8a ships only the zephyr_stub backend (every op returns
 * ALP_ERR_NOT_IMPLEMENTED); real Zephyr-display-driver and V2N
 * DSI / parallel-RGB backends land per the tracking issue on the
 * stub source file.  No vendor extensions exist for display, so
 * the first-member-aliasing pattern the ADC vtable uses is not
 * required here.
 */

#ifndef ALP_BACKENDS_DISPLAY_OPS_H
#define ALP_BACKENDS_DISPLAY_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/display.h>
#include <alp/peripheral.h>

typedef struct alp_display_ops alp_display_ops_t;

/** Backend-owned per-handle state.  The dispatcher caches display_id
 *  here so backends that don't keep their own copy can read it back
 *  at op-time without re-parsing the original alp_display_config_t. */
typedef struct alp_display_backend_state {
	uint32_t                 display_id;
	void                    *be_data;
	const alp_display_ops_t *ops;
} alp_display_backend_state_t;

/** Vtable each display backend implements.  blit args mirror the
 *  public alp_display_blit signature in <alp/display.h> exactly. */
struct alp_display_ops {
	alp_status_t (*open)(const alp_display_config_t  *cfg,
	                     alp_display_backend_state_t *state,
	                     alp_capabilities_t          *caps_out);
	alp_status_t (*get_caps)(alp_display_backend_state_t *state, alp_display_caps_t *out);
	alp_status_t (*blit)(alp_display_backend_state_t *state,
	                     uint16_t                     x,
	                     uint16_t                     y,
	                     uint16_t                     w,
	                     uint16_t                     h,
	                     const void                  *pixels);
	alp_status_t (*clear)(alp_display_backend_state_t *state);
	void (*close)(alp_display_backend_state_t *state);
};

/**
 * Handle struct layout.  Opaque to customers via the public
 * `typedef struct alp_display alp_display_t;` forward declaration in
 * <alp/display.h>.  Defined here so the dispatcher
 * (src/display_dispatch.c) and any future per-backend .c files can
 * access the fields without duplicating the layout.
 */
struct alp_display {
	alp_display_backend_state_t state;
	const alp_backend_t        *backend;
	alp_capabilities_t          cached_caps;
	bool                        in_use;
};

#endif /* ALP_BACKENDS_DISPLAY_OPS_H */
