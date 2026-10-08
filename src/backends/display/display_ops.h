/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_display dispatcher and per-backend
 * implementations.  NOT a public header -- customer code never
 * sees this struct.  Layout may change between SDK versions.
 *
 * Backends in this directory: zephyr_stub.c (wildcard priority-0
 * NOT_IMPLEMENTED fallback), zephyr_drv.c (wildcard priority-50
 * wrapper over Zephyr's drivers/display.h class, issue #23), and
 * yocto_drv.c (priority-100 DRM/KMS dumb-buffer backend on Linux,
 * issue #1143 -- which covers the V2N DU/DSI path through the
 * generic KMS uAPI, so no vendor-specific V2N backend is owed).
 * An Alif LCD-IF backend still lands per the tracking issue on the
 * stub source file.  No vendor extensions exist for display, so the
 * first-member-aliasing pattern the ADC vtable uses is not required
 * here.
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
	/* lifecycle/active_ops drive the generic open/op/close guard in
	 * src/common/alp_slot_claim.h (alp_handle_op_enter/leave/
	 * begin_close, issue #629) -- placed before in_use so the atomic-
	 * claim zeroing in the dispatcher (memset up to
	 * offsetof(..., in_use)) resets both on every fresh claim. */
	uint8_t  lifecycle;
	uint32_t active_ops;
	bool     in_use;
	/* Slot generation, bumped on every fresh claim.  Deliberately AFTER
	 * in_use: the dispatcher's atomic-claim zeroing memsets up to
	 * offsetof(..., in_use), so a counter placed before it would reset on
	 * every claim and never distinguish one owner from the next.
	 *
	 * Exists because a raw `alp_display_t *` is NOT a stable identity:
	 * the pool is static, so after close -> open the same address is a
	 * DIFFERENT display.  A holder that cached the pointer (LVGL's
	 * user-data in src/gui_lvgl.c, issue #1698) must compare epochs to
	 * notice, or it silently draws onto whoever owns the slot now. */
	uint32_t epoch;
};

/**
 * @brief This handle's slot generation (issue #1698).
 *
 * The handle pool is static, so a raw `alp_display_t *` does not identify a
 * display across a close/open pair -- the same address is reused.  A caller
 * that caches the pointer (LVGL's user-data in src/gui_lvgl.c) snapshots this
 * alongside it and re-compares before using the handle; a mismatch means the
 * slot has a new owner.
 *
 * @param[in] h  Display handle, may be NULL.
 * @return the slot generation, or 0 for NULL.
 */
uint32_t alp_display_slot_epoch(const alp_display_t *h);

#endif /* ALP_BACKENDS_DISPLAY_OPS_H */
