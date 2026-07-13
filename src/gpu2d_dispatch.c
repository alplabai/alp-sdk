/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPU2D class dispatcher.  Routes the public alp_gpu2d_* API
 * through the backend registry.
 *
 * Surface validation (NULL pointer, zero dimensions, format range)
 * lives here at the dispatcher so every backend gets consistent
 * INVAL pre-checks before the op fires: invalid surfaces surface
 * as ALP_ERR_INVAL regardless of whether the active backend has a
 * real implementation yet.
 *
 * Handle pool defaults to 1 (CONFIG_ALP_SDK_MAX_GPU2D_HANDLES) --
 * the GPU2D handle is a "system-wide" singleton (one DAVE2D block
 * per SoC).  Customers needing per-core handles can bump the
 * Kconfig.
 *
 * The handle struct layout (struct alp_gpu2d) lives in
 * src/backends/gpu2d/gpu2d_ops.h so per-backend .c files can reach
 * the fields without duplicating the layout.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/gpu2d.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "backends/gpu2d/gpu2d_ops.h"

ALP_BACKEND_DEFINE_CLASS(gpu2d);
/* Pull the gpu2d registry section into a static-archive link (#368). */
ALP_BACKEND_ANCHOR(gpu2d);

/* Reuse the existing TLS-backed last-error mechanism from
 * src/zephyr/last_error.c.  Forward-declared here to avoid pulling
 * in the broader handles.h header (which carries unrelated
 * peripheral pool declarations the dispatcher does not touch). */
#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_GPU2D_HANDLES
#define CONFIG_ALP_SDK_MAX_GPU2D_HANDLES 1
#endif

static struct alp_gpu2d _pool[CONFIG_ALP_SDK_MAX_GPU2D_HANDLES];

/*
 * Issue #629's own reproduction: two threads racing alp_gpu2d_open()
 * against the (default 1-slot) pool used to both win the same slot
 * via an unlocked "if (!in_use) { ...; in_use = true; }" -- 2 pthreads
 * x 200000 iterations reliably produced ~10 duplicate successes.
 * alp_slot_try_claim() makes the flag flip a single atomic
 * compare-exchange, so exactly one caller ever wins a given slot.
 */
static struct alp_gpu2d *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_GPU2D_HANDLES; ++i) {
		/* Atomic claim: only the winner of the flag flip may touch
		 * the slot's other fields (in_use is the struct's last
		 * member, so zero everything before it -- including
		 * lifecycle/active_ops, parking a fresh slot at UNOPENED). */
		if (alp_slot_try_claim(&_pool[i].in_use)) {
			memset(&_pool[i], 0, offsetof(struct alp_gpu2d, in_use));
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_gpu2d *h)
{
	alp_slot_release(&h->in_use);
}

/* Bytes-per-pixel per portable format.  Mirrors the sw_fallback /
 * dave2d tables; kept here (not in gpu2d_ops.h) because the
 * dispatcher needs it for surface validation even when no backend
 * that defines its own copy is compiled in. */
static uint32_t _fmt_bpp(alp_gpu2d_format_t fmt)
{
	switch (fmt) {
	case ALP_GPU2D_FMT_ARGB8888:
	case ALP_GPU2D_FMT_RGBA8888:
		return 4u;
	case ALP_GPU2D_FMT_RGB888:
		return 3u;
	case ALP_GPU2D_FMT_RGB565:
		return 2u;
	case ALP_GPU2D_FMT_A8:
		return 1u;
	default:
		return 0u;
	}
}

static alp_status_t _validate_surface(const alp_gpu2d_surface_t *s)
{
	if (s == NULL) {
		return ALP_ERR_INVAL;
	}
	if (s->base == NULL || s->width == 0u || s->height == 0u || s->stride_bytes == 0u) {
		return ALP_ERR_INVAL;
	}
	if ((unsigned)s->format > (unsigned)ALP_GPU2D_FMT_RGBA8888) {
		return ALP_ERR_INVAL;
	}
	/* A row must fit inside its stride: stride_bytes >= width * bpp.
	 * Checked as a division so a huge width cannot overflow the
	 * multiply into a false pass.  Without this, a malformed stride
	 * makes the sw_fallback's row addressing (y * stride + x * bpp)
	 * step past base + height * stride -- an out-of-bounds write on
	 * caller memory -- and feeds the D/AVE 2D backend a truncated
	 * pixel pitch (see _pitch_px in alif_dave2d.c). */
	if (s->stride_bytes / _fmt_bpp(s->format) < s->width) {
		return ALP_ERR_INVAL;
	}
	return ALP_OK;
}

alp_gpu2d_t *alp_gpu2d_open(void)
{
	alp_z_clear_last_error();
	const alp_backend_t *be = alp_backend_select("gpu2d", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_gpu2d_ops_t *ops = (const alp_gpu2d_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_gpu2d *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	alp_status_t       rc   = ops->open(&h->state, &caps);
	if (rc != ALP_OK) {
		_free(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

alp_status_t alp_gpu2d_fill_rect(alp_gpu2d_t               *h,
                                 const alp_gpu2d_surface_t *dst,
                                 uint32_t                   x,
                                 uint32_t                   y,
                                 uint32_t                   w,
                                 uint32_t                   height,
                                 uint32_t                   argb_color)
{
	/* Gate on the lifecycle byte via alp_handle_op_enter(), not
	 * in_use -- in_use is now touched only by the atomic claim/
	 * release in _alloc/_free (issue #629: mixing an atomic in_use
	 * with a plain read elsewhere is still a data race). A racing
	 * alp_gpu2d_close() cannot free this slot until this op leaves. */
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t s = _validate_surface(dst);
	if (s != ALP_OK) {
		alp_handle_op_leave(&h->active_ops);
		return s;
	}
	alp_status_t rc = h->state.ops->fill_rect(&h->state, dst, x, y, w, height, argb_color);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_gpu2d_blit(alp_gpu2d_t               *h,
                            const alp_gpu2d_surface_t *src,
                            uint32_t                   sx,
                            uint32_t                   sy,
                            const alp_gpu2d_surface_t *dst,
                            uint32_t                   dx,
                            uint32_t                   dy,
                            uint32_t                   w,
                            uint32_t                   height)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t s = _validate_surface(src);
	if (s != ALP_OK) {
		alp_handle_op_leave(&h->active_ops);
		return s;
	}
	s = _validate_surface(dst);
	if (s != ALP_OK) {
		alp_handle_op_leave(&h->active_ops);
		return s;
	}
	alp_status_t rc = h->state.ops->blit(&h->state, src, sx, sy, dst, dx, dy, w, height);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

alp_status_t alp_gpu2d_blend(alp_gpu2d_t               *h,
                             const alp_gpu2d_surface_t *src,
                             uint32_t                   sx,
                             uint32_t                   sy,
                             const alp_gpu2d_surface_t *dst,
                             uint32_t                   dx,
                             uint32_t                   dy,
                             uint32_t                   w,
                             uint32_t                   height,
                             alp_gpu2d_blend_mode_t     mode)
{
	if (h == NULL || !alp_handle_op_enter(&h->lifecycle, &h->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	if ((unsigned)mode > (unsigned)ALP_GPU2D_BLEND_MULTIPLY) {
		alp_handle_op_leave(&h->active_ops);
		return ALP_ERR_INVAL;
	}
	alp_status_t s = _validate_surface(src);
	if (s != ALP_OK) {
		alp_handle_op_leave(&h->active_ops);
		return s;
	}
	s = _validate_surface(dst);
	if (s != ALP_OK) {
		alp_handle_op_leave(&h->active_ops);
		return s;
	}
	alp_status_t rc = h->state.ops->blend(&h->state, src, sx, sy, dst, dx, dy, w, height, mode);
	alp_handle_op_leave(&h->active_ops);
	return rc;
}

void alp_gpu2d_close(alp_gpu2d_t *h)
{
	if (h == NULL) {
		return;
	}
	/* Gate out new ops and drain any in-flight one before touching
	 * state.ops -- makes "close races a blocked/in-flight op" a
	 * bounded wait instead of a use-after-free (issue #629).  Losing
	 * the CAS (already closed/closing/never-opened) makes this a
	 * no-op, matching the existing void-close idempotency contract. */
	if (!alp_handle_begin_close(&h->lifecycle, &h->active_ops)) {
		return;
	}
	if (h->state.ops != NULL && h->state.ops->close != NULL) {
		h->state.ops->close(&h->state);
	}
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free(h);
}

const alp_capabilities_t *alp_gpu2d_capabilities(const alp_gpu2d_t *h)
{
	return (h != NULL) ? &h->cached_caps : NULL;
}
