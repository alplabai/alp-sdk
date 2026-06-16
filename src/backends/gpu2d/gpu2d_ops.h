/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_gpu2d dispatcher and per-backend
 * implementations.  NOT a public header -- customer code never
 * sees this struct.  Layout may change between SDK versions.
 *
 * Backends:
 *   - sw_fallback.c : portable pure-C CPU fill/blit/blend, wildcard
 *                     "*" at priority 0.  open() returns ALP_OK and
 *                     the ops do real pixel work; this is the
 *                     backend native_sim runs and tests.
 *   - alif_dave2d.c : the AEN D/AVE 2D real backend (priority 100,
 *                     per-SKU silicon_refs), gated on the Dave2D
 *                     pack + bench-unverified.
 * No vendor extensions exist for GPU2D, so the first-member-aliasing
 * pattern the ADC vtable uses is not required here.
 */

#ifndef ALP_BACKENDS_GPU2D_OPS_H
#define ALP_BACKENDS_GPU2D_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/gpu2d.h>
#include <alp/peripheral.h>

typedef struct alp_gpu2d_ops alp_gpu2d_ops_t;

/** Backend-owned per-handle state.  GPU2D's public open() takes no
 *  config so the dispatcher mirrors nothing here today; be_data lets
 *  vendor backends hang on whatever HAL state they need. */
typedef struct alp_gpu2d_backend_state {
	void                  *be_data;
	const alp_gpu2d_ops_t *ops;
} alp_gpu2d_backend_state_t;

/** Vtable each GPU2D backend implements.  Op signatures mirror the
 *  public alp_gpu2d_{fill_rect,blit,blend} surface in <alp/gpu2d.h>
 *  exactly so the dispatcher can forward args 1:1 after its surface
 *  validation pre-checks pass. */
struct alp_gpu2d_ops {
	alp_status_t (*open)(alp_gpu2d_backend_state_t *state, alp_capabilities_t *caps_out);
	alp_status_t (*fill_rect)(alp_gpu2d_backend_state_t *state, const alp_gpu2d_surface_t *dst,
	                          uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb_color);
	alp_status_t (*blit)(alp_gpu2d_backend_state_t *state, const alp_gpu2d_surface_t *src,
	                     uint32_t sx, uint32_t sy, const alp_gpu2d_surface_t *dst, uint32_t dx,
	                     uint32_t dy, uint32_t w, uint32_t h);
	alp_status_t (*blend)(alp_gpu2d_backend_state_t *state, const alp_gpu2d_surface_t *src,
	                      uint32_t sx, uint32_t sy, const alp_gpu2d_surface_t *dst, uint32_t dx,
	                      uint32_t dy, uint32_t w, uint32_t h, alp_gpu2d_blend_mode_t mode);
	void (*close)(alp_gpu2d_backend_state_t *state);
};

/**
 * Handle struct layout.  Opaque to customers via the public
 * `typedef struct alp_gpu2d alp_gpu2d_t;` forward declaration in
 * <alp/gpu2d.h>.  Defined here so the dispatcher
 * (src/gpu2d_dispatch.c) and any future per-backend .c files can
 * access the fields without duplicating the layout.
 */
struct alp_gpu2d {
	alp_gpu2d_backend_state_t state;
	const alp_backend_t      *backend;
	alp_capabilities_t        cached_caps;
	bool                      in_use;
};

#endif /* ALP_BACKENDS_GPU2D_OPS_H */
