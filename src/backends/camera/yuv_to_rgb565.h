/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Alp Lab AB
 *
 * Dependency-free BT.601 limited-range YUV -> RGB565 pixel converter.
 *
 * The ISP-Pico MI never produces an RGB output format <alp/camera.h>'s
 * ALP_PIXFMT_RGB565 maps to (zephyr/drivers/video/isp_pico.c's
 * supported_output_fmts is YUV/mono/Bayer only -- see that array's own
 * comment for why RGB888_PLANAR_PRIVATE doesn't count), so
 * alif_isp_pico.c negotiates a YUV MI output instead and converts each
 * pixel on the CPU with this function.  Integer-only (no float, no libm)
 * so it costs the same on the M55 core as it does here.
 *
 * Header-only (mirrors src/backends/adc/adc_oversampling.h's shape) so
 * tests/unit/camera_yuv_to_rgb565 can exercise it hermetically on
 * native_sim -- no video/DT/MMIO involved.
 */

#ifndef ALP_CAMERA_YUV_TO_RGB565_H
#define ALP_CAMERA_YUV_TO_RGB565_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline uint8_t alp_yuv_clip_u8(int32_t v)
{
	if (v < 0) {
		return 0u;
	}
	if (v > 255) {
		return 255u;
	}
	return (uint8_t)v;
}

/**
 * @brief Convert one BT.601 limited-range YCbCr sample to RGB565.
 *
 * @param y  Luma, legal range [16,235].
 * @param u  Cb (blue-difference chroma), legal range [16,240].
 * @param v  Cr (red-difference chroma), legal range [16,240].
 *
 * Standard integer BT.601 (SDTV) decode matrix, rounded (the `+128` bias
 * ahead of each `>>8`) rather than truncated, and clipped to 8 bits per
 * channel before packing down to RGB565's 5/6/5 channel widths.
 *
 * Reference implementation -- one multiply per coefficient, per pixel.
 * tests/unit/camera_yuv_to_rgb565 checks it against known BT.601 primaries
 * AND uses it as the oracle the table-driven fast path below must match
 * bit-exactly. Kept deliberately naive; isp_capture() calls the fast path
 * (alp_yuyv_frame_to_rgb565() / alp_yuv420_frame_to_rgb565()), not this
 * function, on the frame-conversion hot path -- see alif_isp_pico.c's file
 * header for why the CPU conversion cost matters there (AE-convergence
 * bug #ISP-bringup).
 */
static inline uint16_t alp_yuv_to_rgb565(uint8_t y, uint8_t u, uint8_t v)
{
	int32_t c = (int32_t)y - 16;
	int32_t d = (int32_t)u - 128;
	int32_t e = (int32_t)v - 128;

	uint8_t r = alp_yuv_clip_u8((298 * c + 409 * e + 128) >> 8);
	uint8_t g = alp_yuv_clip_u8((298 * c - 100 * d - 208 * e + 128) >> 8);
	uint8_t b = alp_yuv_clip_u8((298 * c + 516 * d + 128) >> 8);

	return (uint16_t)(((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3));
}

/* ============================================================== */
/* Fast path: the exact same BT.601 arithmetic as                 */
/* alp_yuv_to_rgb565() above, factored into per-byte-value lookup */
/* tables so the frame loop below does zero multiplies per pixel. */
/* ============================================================== */

/* _y_term[y]  = 298 * (y-16)         range [-4768, 71222]  -> needs int32_t
 * _rv_term[v] = 409 * (v-128)        range [-52352, 51687] -> needs int32_t
 * _bu_term[u] = 516 * (u-128)        range [-66048, 65532] -> needs int32_t
 * _gu_term[u] = -100 * (u-128)       range [-12700, 12800] -> fits int16_t
 * _gv_term[v] = -208 * (v-128)       range [-26624, 26416] -> fits int16_t
 * The three larger-coefficient terms can't be squeezed into int16_t without
 * either truncating (breaks the bit-exact contract with alp_yuv_to_rgb565())
 * or pre-shifting (changes the rounding the reference performs once, at the
 * end) -- so only the two that actually fit stay compact. */
static int32_t _alp_yuv_y_term[256];
static int32_t _alp_yuv_rv_term[256];
static int32_t _alp_yuv_bu_term[256];
static int16_t _alp_yuv_gu_term[256];
static int16_t _alp_yuv_gv_term[256];
static bool    _alp_yuv_tables_ready;

/** Populate the fast-path tables. Idempotent, cheap (256 iterations, no
 *  division/IO) -- call it before the first alp_yuv_to_rgb565_fast() /
 *  alp_yuyv_frame_to_rgb565() / alp_yuv420_frame_to_rgb565(). */
static inline void alp_yuv_to_rgb565_tables_init(void)
{
	if (_alp_yuv_tables_ready) {
		return;
	}
	for (int32_t i = 0; i < 256; ++i) {
		int32_t c = i - 16;
		int32_t d = i - 128;
		int32_t e = i - 128;

		_alp_yuv_y_term[i]  = 298 * c;
		_alp_yuv_rv_term[i] = 409 * e;
		_alp_yuv_bu_term[i] = 516 * d;
		_alp_yuv_gu_term[i] = (int16_t)(-100 * d);
		_alp_yuv_gv_term[i] = (int16_t)(-208 * e);
	}
	_alp_yuv_tables_ready = true;
}

/** Bit-exact, table-driven twin of alp_yuv_to_rgb565() -- same three sums,
 *  same `+128` rounding bias, same final `>>8` and 5/6/5 pack, just with
 *  each coefficient*sample product pre-computed once per byte value
 *  instead of once per pixel. Requires alp_yuv_to_rgb565_tables_init()
 *  to have run first. */
static inline uint16_t alp_yuv_to_rgb565_fast(uint8_t y, uint8_t u, uint8_t v)
{
	int32_t yt = _alp_yuv_y_term[y];
	uint8_t r  = alp_yuv_clip_u8((yt + _alp_yuv_rv_term[v] + 128) >> 8);
	uint8_t g  = alp_yuv_clip_u8((yt + _alp_yuv_gu_term[u] + _alp_yuv_gv_term[v] + 128) >> 8);
	uint8_t b  = alp_yuv_clip_u8((yt + _alp_yuv_bu_term[u] + 128) >> 8);

	return (uint16_t)(((uint16_t)(r >> 3) << 11) | ((uint16_t)(g >> 2) << 5) | (uint16_t)(b >> 3));
}

/**
 * @brief Convert a packed 4:2:2 YUYV frame to RGB565, fast path.
 *
 * Packed 4:2:2: 4 bytes per pixel PAIR (Y0 U Y1 V), both pixels sharing one
 * chroma sample. `width` MUST be even (true for every size this backend
 * negotiates -- 640 included) so a pixel pair never straddles two rows.
 * `src` and `dst` must both be 4-byte aligned (true for pool-allocated
 * video_buffer memory, see CONFIG_VIDEO_BUFFER_POOL_ALIGN) -- one aligned
 * 32-bit load reads a whole Y0/U/Y1/V group, one aligned 32-bit store
 * writes both output pixels.
 */
static inline void
alp_yuyv_frame_to_rgb565(const uint8_t *src, uint16_t width, uint16_t height, uint16_t *dst)
{
	alp_yuv_to_rgb565_tables_init();

	size_t          n_pairs = ((size_t)width * height) / 2u;
	const uint32_t *src32   = (const uint32_t *)src;
	uint32_t       *dst32   = (uint32_t *)dst;

	for (size_t p = 0; p < n_pairs; ++p) {
		uint32_t word = src32[p];
		uint8_t  y0   = (uint8_t)(word);
		uint8_t  u    = (uint8_t)(word >> 8);
		uint8_t  y1   = (uint8_t)(word >> 16);
		uint8_t  v    = (uint8_t)(word >> 24);

		uint16_t px0 = alp_yuv_to_rgb565_fast(y0, u, v);
		uint16_t px1 = alp_yuv_to_rgb565_fast(y1, u, v);

		dst32[p] = (uint32_t)px0 | ((uint32_t)px1 << 16);
	}
}

/**
 * @brief Convert a planar 4:2:0 YUV420 frame to RGB565, fast path.
 *
 * Planar 4:2:0: one Y plane, then a (width/2)x(height/2) U plane, then a
 * same-size V plane -- 4 luma samples share one chroma sample. `width`
 * MUST be even. `dst` must be 4-byte aligned; two adjacent output pixels
 * (sharing one chroma pair) are written with a single 32-bit store.
 */
static inline void
alp_yuv420_frame_to_rgb565(const uint8_t *src, uint16_t width, uint16_t height, uint16_t *dst)
{
	alp_yuv_to_rgb565_tables_init();

	const uint8_t *y_plane  = src;
	const uint8_t *u_plane  = y_plane + (size_t)width * height;
	const uint8_t *v_plane  = u_plane + (size_t)(width / 2u) * (height / 2u);
	uint16_t       chroma_w = width / 2u;

	for (uint16_t row = 0; row < height; ++row) {
		const uint8_t *y_row        = y_plane + (size_t)row * width;
		const uint8_t *chroma_row_u = u_plane + (size_t)(row / 2u) * chroma_w;
		const uint8_t *chroma_row_v = v_plane + (size_t)(row / 2u) * chroma_w;
		uint32_t      *dst_row      = (uint32_t *)(dst + (size_t)row * width);

		for (uint16_t col = 0; col < width; col += 2u) {
			uint8_t u = chroma_row_u[col / 2u];
			uint8_t v = chroma_row_v[col / 2u];

			uint16_t px0 = alp_yuv_to_rgb565_fast(y_row[col], u, v);
			uint16_t px1 = alp_yuv_to_rgb565_fast(y_row[col + 1u], u, v);

			dst_row[col / 2u] = (uint32_t)px0 | ((uint32_t)px1 << 16);
		}
	}
}

#endif /* ALP_CAMERA_YUV_TO_RGB565_H */
