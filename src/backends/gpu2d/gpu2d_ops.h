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

/** Clip a @p w x @p h rect at (@p x, @p y) to the surface bounds;
 *  returns false (skip the op -- fully clipped, not an error) if the
 *  origin is already past the edge or nothing remains after the clamp.
 *
 *  Shared by every backend so the hardware paths reject/clamp the same
 *  rects the software fallback does: an unclipped rect handed to an
 *  engine is an out-of-bounds DMA write.  The clamp deliberately avoids
 *  computing `x + *w` (which overflows for a caller passing a huge
 *  "fill everything" width, wrapping below s->width and skipping the
 *  clamp); `s->width - x` is safe because `x < s->width` already holds.
 *  Same for h. */
static inline bool
alp_gpu2d_clip_rect(const alp_gpu2d_surface_t *s, uint32_t x, uint32_t y, uint32_t *w, uint32_t *h)
{
	if (x >= s->width || y >= s->height) {
		return false;
	}
	if (*w > s->width - x) {
		*w = s->width - x;
	}
	if (*h > s->height - y) {
		*h = s->height - y;
	}
	return (*w != 0u && *h != 0u);
}

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
	alp_status_t (*fill_rect)(alp_gpu2d_backend_state_t *state,
	                          const alp_gpu2d_surface_t *dst,
	                          uint32_t                   x,
	                          uint32_t                   y,
	                          uint32_t                   w,
	                          uint32_t                   h,
	                          uint32_t                   argb_color);
	alp_status_t (*blit)(alp_gpu2d_backend_state_t *state,
	                     const alp_gpu2d_surface_t *src,
	                     uint32_t                   sx,
	                     uint32_t                   sy,
	                     const alp_gpu2d_surface_t *dst,
	                     uint32_t                   dx,
	                     uint32_t                   dy,
	                     uint32_t                   w,
	                     uint32_t                   h);
	alp_status_t (*blend)(alp_gpu2d_backend_state_t *state,
	                      const alp_gpu2d_surface_t *src,
	                      uint32_t                   sx,
	                      uint32_t                   sy,
	                      const alp_gpu2d_surface_t *dst,
	                      uint32_t                   dx,
	                      uint32_t                   dy,
	                      uint32_t                   w,
	                      uint32_t                   h,
	                      alp_gpu2d_blend_mode_t     mode);
	void (*close)(alp_gpu2d_backend_state_t *state);
};

#if defined(CONFIG_ALP_SDK_GPU2D_SW_FALLBACK)
/**
 * @brief Ops table of the portable software fallback backend.
 * @return The sw_fallback vtable (never NULL).
 *
 * Internal cross-backend hook: a silicon backend whose engine cannot
 * express an op (e.g. D/AVE 2D has no documented single-pass
 * ADDITIVE / MULTIPLY blend) delegates that op to the CPU path
 * instead of returning ALP_ERR_NOSUPPORT, keeping the ADR 0008
 * "write once" contract (the op works, just slower).  The sw ops
 * never touch state->be_data, so a delegating backend may pass its
 * own state through unchanged.  Only compiled when
 * CONFIG_ALP_SDK_GPU2D_SW_FALLBACK selects sw_fallback.c.
 */
const alp_gpu2d_ops_t *alp_gpu2d_sw_ops(void);
#endif /* CONFIG_ALP_SDK_GPU2D_SW_FALLBACK */

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
