/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable software 2D backend for <alp/gpu2d.h>.  Pure C, no
 * hardware, no vendor dependency -- a REAL implementation of
 * fill_rect / blit / blend over caller-owned framebuffer memory.
 *
 * This is the priority-0 wildcard ("*") fallback for the gpu2d
 * class, mirroring the storage / gpio / i2s sw_fallback shape: it
 * always loses backend selection to a real silicon backend (the
 * Alif D/AVE 2D backend registers at priority 100 on AEN parts),
 * but on every other SoM -- and on native_sim -- it is the backend
 * that serves the API.  Unlike the storage sw_fallback (which
 * NOSUPPORTs every data op because there is no portable storage
 * medium), 2D compositing has a fully portable CPU implementation,
 * so this fallback does the real pixel work.  It REPLACES the old
 * NOSUPPORT-only zephyr_stub: open() now returns ALP_OK and the
 * ops touch pixels.
 *
 * Colour model: blit / blend convert between any of the five
 * formats through a common 32-bit ARGB8888 intermediate (unpack ->
 * ARGB8888 -> repack), keeping the converters O(formats).  Lossy
 * conversions are documented, not silent: RGB565 low bits are lost
 * and bit-replicated back; A8 keeps only alpha (RGB reads back 0);
 * and an alpha-less format (RGB888/RGB565) reads as opaque when it
 * is a blend source and drops the composited alpha as a blend dst.
 *
 * @par Cost: ROM ~1.5 KB (the five pack/unpack converters + the four
 *      blend modes); RAM 0 bytes beyond the dispatcher's pool slot
 *      (the backend keeps no per-handle state).
 * @par Performance: O(w * h) per op -- one scalar load + convert +
 *      store per pixel, no SIMD.  Deterministic and allocation-free,
 *      so it is safe to call from any context; it is, however, the
 *      slow path -- silicon builds bind the D/AVE 2D backend instead.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/gpu2d.h>
#include <alp/peripheral.h>

#include "gpu2d_ops.h"

/* ---- bytes-per-pixel ------------------------------------------------ */

static uint32_t _bpp(alp_gpu2d_format_t fmt)
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

/* ---- pixel address helper ------------------------------------------- */

static inline uint8_t *_pix(const alp_gpu2d_surface_t *s, uint32_t x, uint32_t y, uint32_t bpp)
{
	return (uint8_t *)s->base + (size_t)y * s->stride_bytes + (size_t)x * bpp;
}

/* ---- format <-> ARGB8888 intermediate ------------------------------- */

/* Expand a 5- or 6-bit channel to 8 bits by bit-replication (the
 * canonical RGB565 widen, e.g. 5-bit 0x1F -> 0xFF). */
static inline uint8_t _expand5(uint32_t v)
{
	return (uint8_t)((v << 3) | (v >> 2));
}
static inline uint8_t _expand6(uint32_t v)
{
	return (uint8_t)((v << 2) | (v >> 4));
}

/* Read one pixel at p in format fmt, return it as 0xAARRGGBB. */
static uint32_t _unpack(const uint8_t *p, alp_gpu2d_format_t fmt)
{
	switch (fmt) {
	case ALP_GPU2D_FMT_ARGB8888: {
		uint32_t a = p[3], r = p[2], g = p[1], b = p[0]; /* little-endian 0xAARRGGBB */
		return (a << 24) | (r << 16) | (g << 8) | b;
	}
	case ALP_GPU2D_FMT_RGBA8888: {
		uint32_t r = p[3], g = p[2], b = p[1], a = p[0]; /* little-endian 0xRRGGBBAA */
		return (a << 24) | (r << 16) | (g << 8) | b;
	}
	case ALP_GPU2D_FMT_RGB888: {
		uint32_t r = p[2], g = p[1], b = p[0];           /* little-endian B,G,R */
		return (0xFFu << 24) | (r << 16) | (g << 8) | b; /* opaque */
	}
	case ALP_GPU2D_FMT_RGB565: {
		uint32_t v = (uint32_t)p[0] | ((uint32_t)p[1] << 8);
		uint8_t  r = _expand5((v >> 11) & 0x1Fu);
		uint8_t  g = _expand6((v >> 5) & 0x3Fu);
		uint8_t  b = _expand5(v & 0x1Fu);
		return (0xFFu << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b; /* opaque */
	}
	case ALP_GPU2D_FMT_A8:
		return ((uint32_t)p[0] << 24); /* alpha only; RGB = 0 */
	default:
		return 0u;
	}
}

/* Write 0xAARRGGBB to p in format fmt (truncating as the format
 * dictates -- see the lossy-conversion note in the file header). */
static void _pack(uint8_t *p, alp_gpu2d_format_t fmt, uint32_t argb)
{
	uint8_t a = (uint8_t)(argb >> 24);
	uint8_t r = (uint8_t)(argb >> 16);
	uint8_t g = (uint8_t)(argb >> 8);
	uint8_t b = (uint8_t)(argb);
	switch (fmt) {
	case ALP_GPU2D_FMT_ARGB8888:
		p[0] = b;
		p[1] = g;
		p[2] = r;
		p[3] = a;
		break;
	case ALP_GPU2D_FMT_RGBA8888:
		p[0] = a;
		p[1] = b;
		p[2] = g;
		p[3] = r;
		break;
	case ALP_GPU2D_FMT_RGB888:
		p[0] = b;
		p[1] = g;
		p[2] = r;
		break;
	case ALP_GPU2D_FMT_RGB565: {
		uint16_t v = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
		p[0]       = (uint8_t)(v & 0xFFu);
		p[1]       = (uint8_t)(v >> 8);
		break;
	}
	case ALP_GPU2D_FMT_A8:
		p[0] = a;
		break;
	default:
		break;
	}
}

/* ---- per-op clipping ------------------------------------------------ */

/* Clip a w x h rect at (x,y) to the surface; returns false (skip op)
 * if the rect is fully outside or x/y already past the edge. */
static bool _clip(const alp_gpu2d_surface_t *s, uint32_t x, uint32_t y, uint32_t *w, uint32_t *h)
{
	if (x >= s->width || y >= s->height) {
		return false;
	}
	/* Clamp without computing x + *w (which overflows for a caller
     * passing a huge "fill everything" width, wrapping below s->width
     * and skipping the clamp).  s->width - x is safe: x < s->width
     * already holds.  Same for h. */
	if (*w > s->width - x) {
		*w = s->width - x;
	}
	if (*h > s->height - y) {
		*h = s->height - y;
	}
	return (*w != 0u && *h != 0u);
}

/* ---- blend math ----------------------------------------------------- */

static inline uint8_t _u8(uint32_t v)
{
	return (uint8_t)(v > 0xFFu ? 0xFFu : v);
}

/* Composite src over/into dst (both 0xAARRGGBB) per mode. */
static uint32_t _blend_px(uint32_t src, uint32_t dst, alp_gpu2d_blend_mode_t mode)
{
	uint32_t sa = (src >> 24) & 0xFFu, sr = (src >> 16) & 0xFFu, sg = (src >> 8) & 0xFFu,
	         sb = src & 0xFFu;
	uint32_t da = (dst >> 24) & 0xFFu, dr = (dst >> 16) & 0xFFu, dg = (dst >> 8) & 0xFFu,
	         db = dst & 0xFFu;

	switch (mode) {
	case ALP_GPU2D_BLEND_REPLACE:
		return src;

	case ALP_GPU2D_BLEND_SRC_OVER: {
		/* Straight-alpha (non-premultiplied) src-over:
         *   out = src*sa + dst*(1 - sa)
         * so a fully-transparent src (sa=0) leaves dst untouched and
         * a fully-opaque src (sa=255) replaces it.  This is the
         * intuitive surface for the portable API -- callers pass
         * non-premultiplied ARGB.  (The header's premultiplied
         * shorthand `src + dst*(1-sa)` matches only when src is
         * already premultiplied; this backend does the straight-alpha
         * multiply so transparent sources don't bleed.)  Rounded
         * divide by 255: (x + 127) / 255. */
		uint32_t ia  = 255u - sa;
		uint32_t or_ = (sr * sa + dr * ia + 127u) / 255u;
		uint32_t og  = (sg * sa + dg * ia + 127u) / 255u;
		uint32_t ob  = (sb * sa + db * ia + 127u) / 255u;
		uint32_t oa  = sa + (da * ia + 127u) / 255u;
		return (_u8(oa) << 24) | (_u8(or_) << 16) | (_u8(og) << 8) | _u8(ob);
	}

	case ALP_GPU2D_BLEND_ADDITIVE: {
		return (_u8(sa + da) << 24) | (_u8(sr + dr) << 16) | (_u8(sg + dg) << 8) | _u8(sb + db);
	}

	case ALP_GPU2D_BLEND_MULTIPLY: {
		uint32_t or_ = (sr * dr + 127u) / 255u;
		uint32_t og  = (sg * dg + 127u) / 255u;
		uint32_t ob  = (sb * db + 127u) / 255u;
		uint32_t oa  = (sa * da + 127u) / 255u;
		return (_u8(oa) << 24) | (_u8(or_) << 16) | (_u8(og) << 8) | _u8(ob);
	}

	default:
		return src;
	}
}

/* ---- ops ------------------------------------------------------------ */

/**
 * @brief Open the software 2D backend (always succeeds).
 * @param[in,out] state     Backend handle state (unused -- stateless).
 * @param[out]    caps_out  Filled with the instance caps.
 * @return ALP_OK.
 *
 * Caps carry no ALP_INSTANCE_CAP_DMA: this is a CPU implementation,
 * not a DMA engine.  All five pixel formats are supported (there is
 * no per-format cap bit in the instance-cap surface, so support is
 * implicit and documented here rather than advertised in flags).
 */
static alp_status_t sw_open(alp_gpu2d_backend_state_t *state, alp_capabilities_t *caps_out)
{
	state->be_data = NULL;
	if (caps_out != NULL) {
		caps_out->flags               = 0u; /* CPU path: no DMA */
		caps_out->max_sample_rate     = 0u;
		caps_out->max_resolution_bits = 0u;
		caps_out->channel_count       = 0u;
	}
	return ALP_OK;
}

/**
 * @brief Fill a clipped rect with a solid colour (CPU).
 * @param[in]  state      Backend state (unused).
 * @param[in]  dst        Destination surface.
 * @param[in]  x,y        Top-left of the rect.
 * @param[in]  w,h        Rect size (clipped to @p dst).
 * @param[in]  argb_color Fill colour in ARGB8888 (converted to dst fmt).
 * @return ALP_OK, or ALP_ERR_NOSUPPORT for an unknown format.
 */
static alp_status_t sw_fill_rect(alp_gpu2d_backend_state_t *state, const alp_gpu2d_surface_t *dst,
                                 uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                 uint32_t argb_color)
{
	(void)state;
	uint32_t bpp = _bpp(dst->format);
	if (bpp == 0u) {
		return ALP_ERR_NOSUPPORT;
	}
	if (!_clip(dst, x, y, &w, &h)) {
		return ALP_OK; /* fully clipped: nothing to do, not an error */
	}
	for (uint32_t row = 0; row < h; ++row) {
		uint8_t *p = _pix(dst, x, y + row, bpp);
		for (uint32_t col = 0; col < w; ++col) {
			_pack(p, dst->format, argb_color);
			p += bpp;
		}
	}
	return ALP_OK;
}

/**
 * @brief Copy a rect from @p src to @p dst with format conversion (CPU).
 * @param[in] state    Backend state (unused).
 * @param[in] src      Source surface.
 * @param[in] sx,sy    Top-left in @p src.
 * @param[in] dst      Destination surface.
 * @param[in] dx,dy    Top-left in @p dst.
 * @param[in] w,h      Rect size (clipped to BOTH surfaces).
 * @return ALP_OK, or ALP_ERR_NOSUPPORT for an unknown format.
 *
 * The rect is clipped against the source first, then the destination,
 * so neither surface is read or written out of bounds.  Rows are
 * copied top-to-bottom; overlapping same-buffer upward copies are
 * safe, a downward overlapping copy on one buffer can alias (the
 * portable fallback does not reverse-iterate -- callers needing
 * overlap-safe moves should use distinct buffers).
 */
static alp_status_t sw_blit(alp_gpu2d_backend_state_t *state, const alp_gpu2d_surface_t *src,
                            uint32_t sx, uint32_t sy, const alp_gpu2d_surface_t *dst, uint32_t dx,
                            uint32_t dy, uint32_t w, uint32_t h)
{
	(void)state;
	uint32_t sbpp = _bpp(src->format);
	uint32_t dbpp = _bpp(dst->format);
	if (sbpp == 0u || dbpp == 0u) {
		return ALP_ERR_NOSUPPORT;
	}
	if (!_clip(src, sx, sy, &w, &h)) {
		return ALP_OK;
	}
	if (!_clip(dst, dx, dy, &w, &h)) {
		return ALP_OK;
	}
	for (uint32_t row = 0; row < h; ++row) {
		const uint8_t *sp = _pix(src, sx, sy + row, sbpp);
		uint8_t       *dp = _pix(dst, dx, dy + row, dbpp);
		for (uint32_t col = 0; col < w; ++col) {
			_pack(dp, dst->format, _unpack(sp, src->format));
			sp += sbpp;
			dp += dbpp;
		}
	}
	return ALP_OK;
}

/**
 * @brief Alpha-composite @p src into @p dst per @p mode (CPU).
 * @param[in] state    Backend state (unused).
 * @param[in] src      Source surface.
 * @param[in] sx,sy    Top-left in @p src.
 * @param[in] dst      Destination surface.
 * @param[in] dx,dy    Top-left in @p dst.
 * @param[in] w,h      Rect size (clipped to BOTH surfaces).
 * @param[in] mode     One of @ref alp_gpu2d_blend_mode_t.
 * @return ALP_OK, or ALP_ERR_NOSUPPORT for an unknown format.
 *
 * REPLACE is a straight (converting) copy.  SRC_OVER / ADDITIVE /
 * MULTIPLY composite in the ARGB8888 intermediate, then repack to
 * the dst format.  When the dst format carries no alpha (RGB888 /
 * RGB565) the composited alpha is dropped on repack; when the src
 * format carries no alpha its alpha reads as opaque (see header).
 */
static alp_status_t sw_blend(alp_gpu2d_backend_state_t *state, const alp_gpu2d_surface_t *src,
                             uint32_t sx, uint32_t sy, const alp_gpu2d_surface_t *dst, uint32_t dx,
                             uint32_t dy, uint32_t w, uint32_t h, alp_gpu2d_blend_mode_t mode)
{
	(void)state;
	uint32_t sbpp = _bpp(src->format);
	uint32_t dbpp = _bpp(dst->format);
	if (sbpp == 0u || dbpp == 0u) {
		return ALP_ERR_NOSUPPORT;
	}
	if (!_clip(src, sx, sy, &w, &h)) {
		return ALP_OK;
	}
	if (!_clip(dst, dx, dy, &w, &h)) {
		return ALP_OK;
	}
	for (uint32_t row = 0; row < h; ++row) {
		const uint8_t *sp = _pix(src, sx, sy + row, sbpp);
		uint8_t       *dp = _pix(dst, dx, dy + row, dbpp);
		for (uint32_t col = 0; col < w; ++col) {
			uint32_t s = _unpack(sp, src->format);
			uint32_t d = _unpack(dp, dst->format);
			_pack(dp, dst->format, _blend_px(s, d, mode));
			sp += sbpp;
			dp += dbpp;
		}
	}
	return ALP_OK;
}

static const alp_gpu2d_ops_t _ops = {
	.open      = sw_open,
	.fill_rect = sw_fill_rect,
	.blit      = sw_blit,
	.blend     = sw_blend,
	.close     = NULL,
};

ALP_BACKEND_REGISTER(gpu2d, sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
