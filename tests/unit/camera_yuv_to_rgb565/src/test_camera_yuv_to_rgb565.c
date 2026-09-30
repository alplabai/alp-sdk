/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Alif ISP-Pico camera backend converts YUV to RGB565 on the CPU
 * (src/backends/camera/alif_isp_pico.c's isp_capture(), since the ISP MI
 * never produces RGB565 directly -- see that backend's file header).
 * Exercises the pure per-pixel conversion, alp_yuv_to_rgb565(), against
 * the standard BT.601 limited-range primaries -- known (Y,Cb,Cr) triplets
 * for black/white/red/green/blue -- on the host, no video/DT/MMIO
 * involved; and checks that the table-driven fast path isp_capture()
 * actually calls (alp_yuv_to_rgb565_fast() / alp_yuyv_frame_to_rgb565() /
 * alp_yuv420_frame_to_rgb565()) reproduces that same reference bit-exactly.
 */
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "yuv_to_rgb565.h"

static uint8_t rgb565_r5(uint16_t v)
{
	return (v >> 11) & 0x1Fu;
}

static uint8_t rgb565_g6(uint16_t v)
{
	return (v >> 5) & 0x3Fu;
}

static uint8_t rgb565_b5(uint16_t v)
{
	return v & 0x1Fu;
}

ZTEST_SUITE(camera_yuv_to_rgb565, NULL, NULL, NULL, NULL, NULL);

/* BT.601 limited-range (Y in [16,235], Cb/Cr in [16,240]) reference
 * triplets for black/white and the three primaries -- the standard
 * decode-table values (e.g. the ones ffmpeg/most codec docs cite),
 * checked against alp_yuv_to_rgb565()'s output within +-1 LSB per
 * RGB565 channel (5/6/5 bits) to allow for rounding-mode differences. */
ZTEST(camera_yuv_to_rgb565, test_black)
{
	uint16_t got = alp_yuv_to_rgb565(16u, 128u, 128u);

	zassert_within(rgb565_r5(got), 0, 1, "R5 got=%u", rgb565_r5(got));
	zassert_within(rgb565_g6(got), 0, 1, "G6 got=%u", rgb565_g6(got));
	zassert_within(rgb565_b5(got), 0, 1, "B5 got=%u", rgb565_b5(got));
}

ZTEST(camera_yuv_to_rgb565, test_white)
{
	uint16_t got = alp_yuv_to_rgb565(235u, 128u, 128u);

	zassert_within(rgb565_r5(got), 31, 1, "R5 got=%u", rgb565_r5(got));
	zassert_within(rgb565_g6(got), 63, 1, "G6 got=%u", rgb565_g6(got));
	zassert_within(rgb565_b5(got), 31, 1, "B5 got=%u", rgb565_b5(got));
}

ZTEST(camera_yuv_to_rgb565, test_pure_red)
{
	uint16_t got = alp_yuv_to_rgb565(81u, 90u, 240u);

	zassert_within(rgb565_r5(got), 31, 1, "R5 got=%u", rgb565_r5(got));
	zassert_within(rgb565_g6(got), 0, 1, "G6 got=%u", rgb565_g6(got));
	zassert_within(rgb565_b5(got), 0, 1, "B5 got=%u", rgb565_b5(got));
}

ZTEST(camera_yuv_to_rgb565, test_pure_green)
{
	uint16_t got = alp_yuv_to_rgb565(145u, 54u, 34u);

	zassert_within(rgb565_r5(got), 0, 1, "R5 got=%u", rgb565_r5(got));
	zassert_within(rgb565_g6(got), 63, 1, "G6 got=%u", rgb565_g6(got));
	zassert_within(rgb565_b5(got), 0, 1, "B5 got=%u", rgb565_b5(got));
}

ZTEST(camera_yuv_to_rgb565, test_pure_blue)
{
	uint16_t got = alp_yuv_to_rgb565(41u, 240u, 110u);

	zassert_within(rgb565_r5(got), 0, 1, "R5 got=%u", rgb565_r5(got));
	zassert_within(rgb565_g6(got), 0, 1, "G6 got=%u", rgb565_g6(got));
	zassert_within(rgb565_b5(got), 31, 1, "B5 got=%u", rgb565_b5(got));
}

/* --- Fast path: bit-exact against the reference, not +-1 LSB ------------
 *
 * alp_yuv_to_rgb565_fast() and the frame-level alp_yuyv_frame_to_rgb565() /
 * alp_yuv420_frame_to_rgb565() (src/backends/camera/alif_isp_pico.c's
 * isp_capture() hot path) MUST reproduce alp_yuv_to_rgb565() exactly --
 * they're the same three sums and the same `+128`/`>>8` rounding, just with
 * each coefficient*sample product looked up instead of multiplied. Any
 * mismatch here means the table-driven fast path drifted from the
 * reference it's supposed to be a bit-exact twin of. */

ZTEST(camera_yuv_to_rgb565, test_fast_pixel_matches_reference)
{
	/* Every (y,u,v) combination the naive per-triplet loop below would
	 * take hours to cover exhaustively (256^3); this sweeps y and u
	 * fully against a handful of v values instead -- still >65k
	 * comparisons, plenty to catch a table-generation or rounding bug. */
	static const uint8_t v_samples[] = { 0u, 16u, 64u, 128u, 176u, 240u, 255u };

	alp_yuv_to_rgb565_tables_init();

	for (size_t vi = 0; vi < ARRAY_SIZE(v_samples); ++vi) {
		uint8_t v = v_samples[vi];

		for (uint32_t y = 0; y <= 255u; ++y) {
			for (uint32_t u = 0; u <= 255u; ++u) {
				uint16_t want = alp_yuv_to_rgb565((uint8_t)y, (uint8_t)u, v);
				uint16_t got  = alp_yuv_to_rgb565_fast((uint8_t)y, (uint8_t)u, v);

				zassert_equal(
				    got, want, "y=%u u=%u v=%u: fast=0x%04x reference=0x%04x", y, u, v, got, want);
			}
		}
	}
}

/* Small synthetic YUYV frame (8x4, 64 bytes): every byte a distinct,
 * spread-out value so a byte-offset or channel-swap bug in the packed
 * 4:2:2 unpack (word decode in alp_yuyv_frame_to_rgb565()) would show up
 * as a mismatch against the same bytes run one pixel pair at a time
 * through the reference alp_yuv_to_rgb565(). */
ZTEST(camera_yuv_to_rgb565, test_fast_frame_matches_reference_yuyv)
{
#define FRAME_W 8u
#define FRAME_H 4u
	uint8_t  src[FRAME_W * FRAME_H * 2u];
	uint16_t got[FRAME_W * FRAME_H];
	uint16_t want[FRAME_W * FRAME_H];

	for (size_t i = 0; i < ARRAY_SIZE(src); ++i) {
		src[i] = (uint8_t)((i * 37u + 11u) & 0xFFu);
	}

	alp_yuyv_frame_to_rgb565(src, FRAME_W, FRAME_H, got);

	for (size_t p = 0; p < (FRAME_W * FRAME_H) / 2u; ++p) {
		uint8_t y0 = src[4u * p + 0u];
		uint8_t u  = src[4u * p + 1u];
		uint8_t y1 = src[4u * p + 2u];
		uint8_t v  = src[4u * p + 3u];

		want[2u * p]      = alp_yuv_to_rgb565(y0, u, v);
		want[2u * p + 1u] = alp_yuv_to_rgb565(y1, u, v);
	}

	zassert_mem_equal(got, want, sizeof(got), "fast YUYV frame path drifted from the reference");
#undef FRAME_W
#undef FRAME_H
}

/* Small synthetic planar 4:2:0 frame (8x4): one Y plane (32 bytes), a
 * 4x2 U plane (8 bytes), a 4x2 V plane (8 bytes) -- exercises the plane
 * offsets AND the row-wise 2x2 chroma sharing in
 * alp_yuv420_frame_to_rgb565() against the same per-pixel reference
 * calls the YUYV test above uses. */
ZTEST(camera_yuv_to_rgb565, test_fast_frame_matches_reference_yuv420)
{
#define FRAME_W  8u
#define FRAME_H  4u
#define CHROMA_W (FRAME_W / 2u)
#define CHROMA_H (FRAME_H / 2u)
	uint8_t  y_plane[FRAME_W * FRAME_H];
	uint8_t  u_plane[CHROMA_W * CHROMA_H];
	uint8_t  v_plane[CHROMA_W * CHROMA_H];
	uint8_t  src[sizeof(y_plane) + sizeof(u_plane) + sizeof(v_plane)];
	uint16_t got[FRAME_W * FRAME_H];
	uint16_t want[FRAME_W * FRAME_H];

	for (size_t i = 0; i < ARRAY_SIZE(y_plane); ++i) {
		y_plane[i] = (uint8_t)((i * 41u + 3u) & 0xFFu);
	}
	for (size_t i = 0; i < ARRAY_SIZE(u_plane); ++i) {
		u_plane[i] = (uint8_t)((i * 59u + 71u) & 0xFFu);
		v_plane[i] = (uint8_t)((i * 67u + 131u) & 0xFFu);
	}
	memcpy(src, y_plane, sizeof(y_plane));
	memcpy(src + sizeof(y_plane), u_plane, sizeof(u_plane));
	memcpy(src + sizeof(y_plane) + sizeof(u_plane), v_plane, sizeof(v_plane));

	alp_yuv420_frame_to_rgb565(src, FRAME_W, FRAME_H, got);

	for (uint16_t row = 0; row < FRAME_H; ++row) {
		for (uint16_t col = 0; col < FRAME_W; ++col) {
			uint8_t y = y_plane[(size_t)row * FRAME_W + col];
			uint8_t u = u_plane[(size_t)(row / 2u) * CHROMA_W + col / 2u];
			uint8_t v = v_plane[(size_t)(row / 2u) * CHROMA_W + col / 2u];

			want[(size_t)row * FRAME_W + col] = alp_yuv_to_rgb565(y, u, v);
		}
	}

	zassert_mem_equal(got, want, sizeof(got), "fast YUV420 frame path drifted from the reference");
#undef FRAME_W
#undef FRAME_H
#undef CHROMA_W
#undef CHROMA_H
}
