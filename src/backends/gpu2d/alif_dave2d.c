/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif D/AVE 2D real backend for <alp/gpu2d.h> (vendor-ext).
 *
 * The AEN-family 2D accelerator is the TES D/AVE 2D engine (Alif's
 * marketing name "GPU2D").  This backend drives it through the
 * documented d2_* C API of the Alif D/AVE 2D driver
 * (github.com/alifsemi/alif_dave2d-driver, CMSIS pack
 * AlifSemiconductor::Dave2DDriver).  That pack is PROPRIETARY and is
 * pulled at build time only -- it is NOT vendored into alp-sdk.  The
 * whole translation unit is therefore gated behind
 * CONFIG_ALP_SDK_GPU2D_ALIF_DAVE2D, which the build only sets when
 * the pack is on the include path (mirrors the ALP_HAS_ALIF_HAL
 * model used by the vendors/alif/ sources).
 *
 * Bench-verified on E1M-AEN803 2026W36-0001 (M55-HE) against the driver at
 * the `vendor-sdks` west pin: fill, blit and REPLACE / SRC_OVER blends
 * match the software fallback (SRC_OVER within 1 per channel, see
 * <alp/gpu2d.h>); ADDITIVE / MULTIPLY are served by the fallback. Three
 * things the first cut got wrong, all found on silicon:
 *   - d2_startframe() starts the PREVIOUS frame's render buffer, so every
 *     op is followed by an empty frame (_submit_and_wait());
 *   - d2_setcolor() carries no alpha, and the default colour blend is
 *     alpha / one-minus-alpha, so fills and copies set a plain store;
 *   - the texture alpha is modulated by the context's constant alpha.
 * The engine is an AXI master with no address translation in the driver:
 * surfaces and the driver heap must be at globally visible addresses (the
 * SRAM0 bank on the M55-HE, not DTCM). Surfaces get cache maintenance around
 * each op (_cache_pre/_cache_post). The backend requires CONFIG_DCACHE: with
 * the D-cache off the engine's output never reached the CPU on the bench, and
 * the software fallback serves <alp/gpu2d.h> in that configuration.
 *
 * Coherency / cache maintenance against the caller's framebuffer
 * (clean src before read, invalidate dst after write -- see
 * docs/aen-accelerator-backends-design.md §1 "Coherency") is a
 * documented bench follow-up; it is flagged at each op below rather
 * than guessed at here.
 */

#include <alp/peripheral.h>

#if defined(CONFIG_ALP_SDK_GPU2D_ALIF_DAVE2D)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/gpu2d.h>

#include "gpu2d_ops.h"

/* Documented D/AVE 2D driver API.  Provided by the build-time pack;
 * never vendored here. */
#include "dave_driver.h"
#include "dave_registermap.h"

#include <zephyr/cache.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/barrier.h>

#if defined(CONFIG_D1_MALLOC_D0LIB)
#include <zephyr/devicetree.h>
#include <zephyr/linker/devicetree_regions.h>
#include <zephyr/sys/util.h>

#include "dave_d0lib.h"

/*
 * The driver's display lists live in this heap, and the engine is an AXI
 * master that reads them at the address the CPU sees: the driver does no
 * CPU-to-bus translation (d1_maptovidmem() is a stub). On the M55-HE the
 * default data section is core-local DTCM, which the engine cannot reach, so
 * the heap goes in the global SRAM0 bank when the devicetree has one.
 * Surfaces handed to this backend have the same requirement.
 */
#if DT_NODE_EXISTS(DT_NODELABEL(sram0))
#define DAVE2D_HEAP_SECTION Z_GENERIC_SECTION(LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(sram0)))
#else
#define DAVE2D_HEAP_SECTION
#endif
static uint8_t
    DAVE2D_HEAP_SECTION __aligned(32) _d0_heap[CONFIG_ALP_SDK_GPU2D_ALIF_DAVE2D_HEAP_SIZE];
static bool             _d0_heap_ready;

/* d2_opendevice() allocates through the D0 heap manager, which must be set up
 * once before the first device is opened. */
static bool _d0_heap_init_once(void)
{
	if (!_d0_heap_ready) {
		_d0_heap_ready = d0_initheapmanager(
		    _d0_heap, sizeof(_d0_heap), d0_mm_fixed_range, NULL, 0, 0, 0, d0_ma_unified);
	}
	return _d0_heap_ready;
}
#else
static bool _d0_heap_init_once(void)
{
	return true;
}
#endif

/**
 * @brief Map an alp_gpu2d_format_t to a D/AVE 2D d2_mode_* constant.
 * @param[in]  fmt   Portable pixel format.
 * @param[out] out   D/AVE 2D colour mode on success.
 * @return ALP_OK if the format maps, ALP_ERR_NOSUPPORT otherwise.
 *
 * RGBA8888 has no distinct D/AVE 2D channel order from ARGB8888 in
 * the driver's documented mode set, so it maps to the same
 * d2_mode_argb8888 here; the byte-order difference is a bench
 * follow-up (the engine's source-colour-key + global-alpha path may
 * need an explicit swizzle).
 */
/**
 * @brief Bytes-per-pixel for a portable format (0 if unmapped).
 * @param[in] fmt  Portable pixel format.
 * @return Bytes per pixel, or 0 for an unknown format.
 *
 * Used to derive the D/AVE 2D pixel pitch from a byte stride directly,
 * instead of the fragile `stride / (stride / width)` round-trip (which
 * truncates on a padded stride and divides by zero when
 * stride_bytes < width, a case the dispatcher's surface validation
 * does not reject).
 */
static d2_s32 _bpp_for(alp_gpu2d_format_t fmt)
{
	switch (fmt) {
	case ALP_GPU2D_FMT_ARGB8888:
	case ALP_GPU2D_FMT_RGBA8888:
		return 4;
	case ALP_GPU2D_FMT_RGB888:
		return 3;
	case ALP_GPU2D_FMT_RGB565:
		return 2;
	case ALP_GPU2D_FMT_A8:
		return 1;
	default:
		return 0;
	}
}

/* Pixel pitch (in pixels) from a surface's byte stride.  bpp is
 * non-zero whenever the format mapped through _fmt_to_d2 first. */
static d2_s32 _pitch_px(const alp_gpu2d_surface_t *s)
{
	d2_s32 bpp = _bpp_for(s->format);
	return bpp != 0 ? (d2_s32)(s->stride_bytes / (d2_u32)bpp) : 0;
}

/*
 * Coordinate guards for the fixed-point submit path.  The ops below
 * shift pixel coordinates <<4 into the engine's n.4 fixed-point
 * d2_point / d2_width and pass blit origins as d2_blitpos; an
 * unguarded uint32_t would silently truncate/wrap in those casts and
 * the engine would render at a garbage offset.  Both limits are
 * derived from the pack's own typedef width (sizeof), so a pack
 * revision that widens the types widens the guard with it -- no
 * hardware range value is invented here.
 *
 * D2_FIXED4_MAX: largest pixel value whose <<4 still fits the SIGNED
 * positive range of the type.  D2_POS_MAX: largest unshifted pixel
 * value for the type (signed positive range assumed -- conservative
 * by half if a pack revision makes it unsigned).
 */
#define D2_FIXED4_MAX(type) ((uint32_t)(((UINT64_C(1) << (sizeof(type) * 8u - 1u)) - 1u) >> 4))
#define D2_POS_MAX(type)    ((uint32_t)((UINT64_C(1) << (sizeof(type) * 8u - 1u)) - 1u))

static alp_status_t _fmt_to_d2(alp_gpu2d_format_t fmt, d2_u32 *out)
{
	switch (fmt) {
	case ALP_GPU2D_FMT_ARGB8888:
	case ALP_GPU2D_FMT_RGBA8888:
		*out = d2_mode_argb8888;
		return ALP_OK;
	case ALP_GPU2D_FMT_RGB565:
		*out = d2_mode_rgb565;
		return ALP_OK;
	case ALP_GPU2D_FMT_RGB888:
		*out = d2_mode_rgb888;
		return ALP_OK;
	case ALP_GPU2D_FMT_A8:
		*out = d2_mode_alpha8;
		return ALP_OK;
	default:
		return ALP_ERR_NOSUPPORT;
	}
}

/**
 * @brief Map a portable blend mode to a D/AVE 2D (src,dst) factor pair.
 * @param[in]  mode    Portable blend mode.
 * @param[out] src_bf  D/AVE 2D source blend factor.
 * @param[out] dst_bf  D/AVE 2D destination blend factor.
 * @return ALP_OK if the mode maps to a documented d2_blendmode pair,
 *         ALP_ERR_NOSUPPORT otherwise.
 *
 * REPLACE and SRC_OVER map cleanly onto the documented
 * d2_setblendmode factor pairs.  ADDITIVE and MULTIPLY do NOT have a
 * documented single-pass d2_setblendmode equivalent (the driver's
 * fixed blend stage is a Porter-Duff factor multiply-add, not a
 * channel product or a saturating add of dst), so dave2d_blend never
 * routes them here -- it delegates them to the software fallback's
 * CPU path (or returns NOSUPPORT when sw_fallback is compiled out).
 * Revisit if a bench check shows the engine's extended-blend path
 * covers them.  BENCH-UNVERIFIED.
 */
static alp_status_t _blend_to_d2(alp_gpu2d_blend_mode_t mode,
                                 d2_u32                *src_bf,
                                 d2_u32                *dst_bf,
                                 d2_u32                *src_abf,
                                 d2_u32                *dst_abf)
{
	switch (mode) {
	case ALP_GPU2D_BLEND_REPLACE:
		*src_bf  = d2_bm_one;
		*dst_bf  = d2_bm_zero;
		*src_abf = d2_bm_one;
		*dst_abf = d2_bm_zero;
		return ALP_OK;
	case ALP_GPU2D_BLEND_SRC_OVER:
		/* Straight (non-premultiplied) alpha, matching sw_fallback:
		 * rgb = src*a + dst*(1-a), a = a_src + a_dst*(1-a_src). */
		*src_bf  = d2_bm_alpha;
		*dst_bf  = d2_bm_one_minus_alpha;
		*src_abf = d2_bm_one;
		*dst_abf = d2_bm_one_minus_alpha;
		return ALP_OK;
	case ALP_GPU2D_BLEND_ADDITIVE:
	case ALP_GPU2D_BLEND_MULTIPLY:
		/* No single-pass d2 factor pair; dave2d_blend delegates these to
		 * the sw path before reaching here. */
		return ALP_ERR_NOSUPPORT;
	default:
		return ALP_ERR_NOSUPPORT;
	}
}

/* Plain store of source colour and alpha: fills and copies must not blend
 * with what is already there (D/AVE's default colour blend is
 * alpha / one-minus-alpha). */
static void _set_store(d2_device *dev)
{
	d2_setblendmode(dev, d2_bm_one, d2_bm_zero);
	d2_setalphablendmode(dev, d2_bm_one, d2_bm_zero);
}

/**
 * @brief Bind @p s as the active D/AVE 2D render target.
 * @param[in] dev  D/AVE 2D device handle.
 * @param[in] s    Surface to bind as the framebuffer.
 * @return ALP_OK on success, ALP_ERR_NOSUPPORT for an unmapped format.
 */
static alp_status_t _bind_dst(d2_device *dev, const alp_gpu2d_surface_t *s)
{
	d2_u32       mode;
	alp_status_t rc = _fmt_to_d2(s->format, &mode);
	if (rc != ALP_OK) {
		return rc;
	}
	/* d2_framebuffer(handle, ptr, pitch-in-pixels, width, height, mode) */
	d2_framebuffer(dev, s->base, _pitch_px(s), (d2_u32)s->width, (d2_u32)s->height, mode);
	return ALP_OK;
}

/*
 * The engine reads and writes surfaces behind the CPU's data cache. Before an
 * op: write back dirty source/destination lines so the engine sees the CPU's
 * pixels, and drop the destination's so no dirty line is written over the
 * engine's output later. After it: drop the destination again so the CPU
 * reads the engine's pixels, not stale lines. No-ops without CONFIG_DCACHE.
 * Bench (D-cache on, before this): fills and blits read back the previous
 * op's colour.
 */
static size_t _surf_bytes(const alp_gpu2d_surface_t *s)
{
	return (size_t)s->stride_bytes * (size_t)s->height;
}

static void _cache_pre(const alp_gpu2d_surface_t *dst, const alp_gpu2d_surface_t *src)
{
	if (src != NULL) {
		(void)sys_cache_data_flush_range(src->base, _surf_bytes(src));
	}
	(void)sys_cache_data_flush_and_invd_range(dst->base, _surf_bytes(dst));
}

static void _cache_post(const alp_gpu2d_surface_t *dst)
{
	(void)sys_cache_data_invd_range(dst->base, _surf_bytes(dst));
}

/*
 * Submit the commands recorded since d2_startframe() and wait for the engine
 * to finish them. d2_startframe() starts the render buffer closed by the
 * PREVIOUS d2_endframe(), and d2_endframe() waits only for that previous one
 * (dave_rbuffer.c), so a lone start/end pair leaves each op's pixels unwritten
 * until the next op -- bench-observed as every result arriving one call late.
 * Starting and ending an empty frame kicks this op's buffer and waits for it.
 */
/** Upper bound on the engine write-back drain after a submitted op. */
#define DAVE2D_IDLE_TIMEOUT_MS 100

static alp_status_t _submit_and_wait(d2_device *dev)
{
	d2_endframe(dev);
	/* The display list is Normal memory the CPU just wrote; the kick is a
	 * Device register write. Without a DSB the engine can start on a list
	 * whose last entries are still in the CPU's write buffer -- bench, with
	 * the D-cache off (no cache maintenance to supply the barrier): every
	 * result one op late. */
	barrier_dsync_fence_full();
	d2_startframe(dev);
	d2_endframe(dev);
	/*
	 * d2_endframe() returns when the display list is done (D2C_DLISTACTIVE
	 * clear), not when the engine has written its last pixels out. With the
	 * D-cache off the CPU read the surface before the write-back drained and
	 * saw the previous op's colour; wait for the enumeration and write-back
	 * units too.
	 */
	d1_device *hw = d2_level1interface(dev);

	/* Bounded: a wedged or bus-faulted engine must not hang the caller. */
	const int64_t deadline = k_uptime_get() + DAVE2D_IDLE_TIMEOUT_MS;

	while ((d1_getregister(hw, D1_DAVE2D, D2_STATUS) & (D2C_BUSY_ENUM | D2C_BUSY_WRITE)) != 0) {
		if (k_uptime_get() > deadline) return ALP_ERR_TIMEOUT;
	}
	return ALP_OK;
}

static alp_status_t dave2d_open(alp_gpu2d_backend_state_t *state, alp_capabilities_t *caps_out)
{
	if (!_d0_heap_init_once()) {
		return ALP_ERR_NOMEM;
	}
	d2_device *dev = d2_opendevice(0);
	if (dev == NULL) {
		return ALP_ERR_NOSUPPORT;
	}
	if (d2_inithw(dev, 0) != D2_OK) {
		d2_closedevice(dev);
		return ALP_ERR_NOSUPPORT;
	}
	state->be_data = dev;
	if (caps_out != NULL) {
		/* DMA blit engine -- vendor-IP identity, genuinely known here. */
		caps_out->flags               = ALP_INSTANCE_CAP_DMA | ALP_INSTANCE_CAP_REPORTED;
		caps_out->max_rate_hz         = 0u;
		caps_out->max_resolution_bits = 0u;
		caps_out->channel_count       = 0u;
	}
	return ALP_OK;
}

static alp_status_t dave2d_fill_rect(alp_gpu2d_backend_state_t *state,
                                     const alp_gpu2d_surface_t *dst,
                                     uint32_t                   x,
                                     uint32_t                   y,
                                     uint32_t                   w,
                                     uint32_t                   h,
                                     uint32_t                   argb_color)
{
	d2_device   *dev = (d2_device *)state->be_data;
	alp_status_t rc  = _bind_dst(dev, dst);
	if (rc != ALP_OK) {
		return rc;
	}
	/* Clip to the dst surface exactly like the sw_fallback does (shared
	 * helper in gpu2d_ops.h): an unclipped rect handed to the engine is
	 * an out-of-bounds DMA write into whatever follows the framebuffer. */
	if (!alp_gpu2d_clip_rect(dst, x, y, &w, &h)) {
		return ALP_OK; /* fully clipped: nothing to do, not an error */
	}
	/* Reject coordinates whose 16.4 fixed-point encoding (<<4 below)
	 * would overflow d2_point / d2_width -- the casts are otherwise
	 * silent.  x + w never wraps: both are clipped <= dst->width. */
	if ((uint64_t)x + w > D2_FIXED4_MAX(d2_point) || (uint64_t)y + h > D2_FIXED4_MAX(d2_point) ||
	    w > D2_FIXED4_MAX(d2_width) || h > D2_FIXED4_MAX(d2_width)) {
		return ALP_ERR_OUT_OF_RANGE;
	}
	/* BENCH-UNVERIFIED: clean the dst range from cache after the
     * engine writes it (docs/aen-accelerator-backends-design.md §1). */
	_cache_pre(dst, NULL);
	d2_startframe(dev);
	_set_store(dev);
	d2_setcolor(dev, 0, (d2_color)argb_color);
	/* d2_setcolor() carries RGB only; the written alpha is the constant
	 * alpha, 0xff unless set -- bench-observed as every fill landing opaque. */
	d2_setalpha(dev, (d2_alpha)(argb_color >> 24));
	d2_renderbox(dev,
	             (d2_point)(x << 4),
	             (d2_point)(y << 4),
	             (d2_width)(w << 4),
	             (d2_width)(h << 4));        /* 16.4 fixed point */
	alp_status_t ws = _submit_and_wait(dev); /* submit-and-wait per the v0.5 API contract */
	_cache_post(dst);
	return ws;
}

static alp_status_t dave2d_blit(alp_gpu2d_backend_state_t *state,
                                const alp_gpu2d_surface_t *src,
                                uint32_t                   sx,
                                uint32_t                   sy,
                                const alp_gpu2d_surface_t *dst,
                                uint32_t                   dx,
                                uint32_t                   dy,
                                uint32_t                   w,
                                uint32_t                   h)
{
	d2_device   *dev = (d2_device *)state->be_data;
	d2_u32       src_mode;
	alp_status_t rc = _fmt_to_d2(src->format, &src_mode);
	if (rc != ALP_OK) {
		return rc;
	}
	rc = _bind_dst(dev, dst);
	if (rc != ALP_OK) {
		return rc;
	}
	/* Clip against BOTH surfaces (same order/shape as sw_fallback):
	 * the engine reads sx..sx+w from src and writes dx..dx+w into
	 * dst, and neither walk may leave its surface. */
	if (!alp_gpu2d_clip_rect(src, sx, sy, &w, &h)) {
		return ALP_OK; /* fully clipped: nothing to do, not an error */
	}
	if (!alp_gpu2d_clip_rect(dst, dx, dy, &w, &h)) {
		return ALP_OK;
	}
	/* Fixed-point overflow guard: dx/dy/w/h are shifted <<4 into
	 * d2_point / d2_width; sx/sy pass unshifted as d2_blitpos. */
	if ((uint64_t)sx + w > D2_POS_MAX(d2_blitpos) || (uint64_t)sy + h > D2_POS_MAX(d2_blitpos) ||
	    (uint64_t)dx + w > D2_FIXED4_MAX(d2_point) || (uint64_t)dy + h > D2_FIXED4_MAX(d2_point) ||
	    w > D2_FIXED4_MAX(d2_width) || h > D2_FIXED4_MAX(d2_width)) {
		return ALP_ERR_OUT_OF_RANGE;
	}
	_cache_pre(dst, src);
	d2_startframe(dev);
	_set_store(dev);
	d2_setalpha(dev, 0xff); /* see dave2d_blend() */
	d2_setblitsrc(
	    dev, src->base, _pitch_px(src), (d2_u32)src->width, (d2_u32)src->height, src_mode);
	d2_blitcopy(dev,
	            (d2_s32)w,
	            (d2_s32)h,
	            (d2_blitpos)sx,
	            (d2_blitpos)sy,
	            (d2_width)(w << 4),
	            (d2_width)(h << 4),
	            (d2_point)(dx << 4),
	            (d2_point)(dy << 4),
	            0);
	alp_status_t ws = _submit_and_wait(dev);
	_cache_post(dst);
	return ws;
}

static alp_status_t dave2d_blend(alp_gpu2d_backend_state_t *state,
                                 const alp_gpu2d_surface_t *src,
                                 uint32_t                   sx,
                                 uint32_t                   sy,
                                 const alp_gpu2d_surface_t *dst,
                                 uint32_t                   dx,
                                 uint32_t                   dy,
                                 uint32_t                   w,
                                 uint32_t                   h,
                                 alp_gpu2d_blend_mode_t     mode)
{
	d2_device *dev = (d2_device *)state->be_data;
	d2_u32     src_bf, dst_bf, src_abf, dst_abf, src_mode;

	if (mode == ALP_GPU2D_BLEND_ADDITIVE || mode == ALP_GPU2D_BLEND_MULTIPLY) {
#if defined(CONFIG_ALP_SDK_GPU2D_SW_FALLBACK)
		/* The engine has no documented single-pass mapping for these
	     * two modes (see _blend_to_d2); serve them from the portable
	     * CPU path so the ADR 0008 "write once" contract holds on AEN
	     * too.  The sw ops ignore be_data, so our state passes through
	     * unchanged.  BENCH-UNVERIFIED coherency caveat: the CPU
	     * composite reads/writes caller memory directly -- the same
	     * cache-maintenance follow-up flagged in the file header
	     * applies where it mixes with prior engine writes. */
		return alp_gpu2d_sw_ops()->blend(state, src, sx, sy, dst, dx, dy, w, h, mode);
#else
		return ALP_ERR_NOSUPPORT; /* sw_fallback compiled out */
#endif
	}

	alp_status_t rc = _blend_to_d2(mode, &src_bf, &dst_bf, &src_abf, &dst_abf);
	if (rc != ALP_OK) {
		return rc;
	}
	rc = _fmt_to_d2(src->format, &src_mode);
	if (rc != ALP_OK) {
		return rc;
	}
	rc = _bind_dst(dev, dst);
	if (rc != ALP_OK) {
		return rc;
	}
	/* Same clip + fixed-point guard as dave2d_blit -- the blend path
	 * feeds the identical d2_blitcopy coordinate set. */
	if (!alp_gpu2d_clip_rect(src, sx, sy, &w, &h)) {
		return ALP_OK; /* fully clipped: nothing to do, not an error */
	}
	if (!alp_gpu2d_clip_rect(dst, dx, dy, &w, &h)) {
		return ALP_OK;
	}
	if ((uint64_t)sx + w > D2_POS_MAX(d2_blitpos) || (uint64_t)sy + h > D2_POS_MAX(d2_blitpos) ||
	    (uint64_t)dx + w > D2_FIXED4_MAX(d2_point) || (uint64_t)dy + h > D2_FIXED4_MAX(d2_point) ||
	    w > D2_FIXED4_MAX(d2_width) || h > D2_FIXED4_MAX(d2_width)) {
		return ALP_ERR_OUT_OF_RANGE;
	}
	_cache_pre(dst, src);
	d2_startframe(dev);
	d2_setblendmode(dev, src_bf, dst_bf);
	d2_setalphablendmode(dev, src_abf, dst_abf);
	/* The texture's alpha is modulated by the context's constant alpha,
	 * which a previous fill may have left below 0xff. */
	d2_setalpha(dev, 0xff);
	d2_setblitsrc(
	    dev, src->base, _pitch_px(src), (d2_u32)src->width, (d2_u32)src->height, src_mode);
	d2_blitcopy(dev,
	            (d2_s32)w,
	            (d2_s32)h,
	            (d2_blitpos)sx,
	            (d2_blitpos)sy,
	            (d2_width)(w << 4),
	            (d2_width)(h << 4),
	            (d2_point)(dx << 4),
	            (d2_point)(dy << 4),
	            d2_bf_usealpha);
	alp_status_t ws = _submit_and_wait(dev);
	_cache_post(dst);
	return ws;
}

static void dave2d_close(alp_gpu2d_backend_state_t *state)
{
	d2_device *dev = (d2_device *)state->be_data;
	if (dev != NULL) {
		d2_deinithw(dev);
		d2_closedevice(dev);
		state->be_data = NULL;
	}
}

static const alp_gpu2d_ops_t _ops = {
	.open      = dave2d_open,
	.fill_rect = dave2d_fill_rect,
	.blit      = dave2d_blit,
	.blend     = dave2d_blend,
	.close     = dave2d_close,
};

/* One row per AEN SKU that actually carries the D/AVE 2D engine
 * (`dave2d: true` in metadata/socs/alif/ensemble/<sku>.json -- only
 * e6, e7, e8; e3/e4/e5 set it false/absent and e1/e2 do not exist as
 * variants), priority 100 so the registry prefers it over the
 * wildcard sw_fallback (priority 0) on those parts.  Same multi-SKU
 * registration pattern as src/backends/inference/ethos_u_aen.cpp. */
ALP_BACKEND_REGISTER(gpu2d,
                     dave2d_e6,
                     {
                         .silicon_ref = "alif:ensemble:e6",
                         .vendor      = "alif",
                         .base_caps = (uint32_t)(ALP_INSTANCE_CAP_DMA | ALP_INSTANCE_CAP_REPORTED),
                         .priority  = 100,
                         .ops       = &_ops,
                         .probe     = NULL,
                     });

ALP_BACKEND_REGISTER(gpu2d,
                     dave2d_e7,
                     {
                         .silicon_ref = "alif:ensemble:e7",
                         .vendor      = "alif",
                         .base_caps = (uint32_t)(ALP_INSTANCE_CAP_DMA | ALP_INSTANCE_CAP_REPORTED),
                         .priority  = 100,
                         .ops       = &_ops,
                         .probe     = NULL,
                     });

ALP_BACKEND_REGISTER(gpu2d,
                     dave2d_e8,
                     {
                         .silicon_ref = "alif:ensemble:e8",
                         .vendor      = "alif",
                         .base_caps = (uint32_t)(ALP_INSTANCE_CAP_DMA | ALP_INSTANCE_CAP_REPORTED),
                         .priority  = 100,
                         .ops       = &_ops,
                         .probe     = NULL,
                     });

#endif /* CONFIG_ALP_SDK_GPU2D_ALIF_DAVE2D */
