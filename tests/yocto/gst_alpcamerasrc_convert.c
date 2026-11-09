/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file gst_alpcamerasrc_convert.c
 * @brief Synthetic CR10 frame -> SDK unpack -> alpcamerasrc layouts, byte for byte.
 *
 * Packs a gradient into the CRU's CR10 layout (6 px per LE 64-bit word, rows
 * padded to the CRU's w*8 stride), unpacks it with the same row routine
 * yocto_drv.c uses, then checks GRAY8 / GRAY16_LE / NV12 with 4-byte-padded
 * GStreamer strides, including the canary bytes past each row.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "alp_gst_convert.h"
#include "cam_cr10_unpack.h"
#include "test_assert.h"

#define W 13u /* odd-ish width: not a multiple of 6, 4 or 24 */
#define H 4u

static uint32_t sample(uint32_t x, uint32_t y)
{
	return (x * 83u + y * 301u) & 0x3FFu;
}

static void pack_frame(uint8_t *src, size_t stride)
{
	memset(src, 0, stride * H);
	for (uint32_t y = 0; y < H; ++y) {
		for (uint32_t x = 0; x < W; ++x) {
			uint64_t word;
			uint8_t *p = src + y * stride + (x / 6u) * 8u;
			memcpy(&word, p, 8u);
			word |= (uint64_t)sample(x, y) << ((x % 6u) * 10u);
			memcpy(p, &word, 8u);
		}
	}
}

/* Bayer: build a frame of uniform 2x2-output-pixel colour blocks in every order. */
static void
bayer_set(uint16_t *f, uint32_t sw, uint32_t px, uint32_t py, int order, const uint16_t c[3])
{
	/* c = R,G,B; quad slots (r0c0, r0c1, r1c0, r1c1) per order */
	static const char *const lay[4] = { "RGGB", "BGGR", "GRBG", "GBRG" };
	for (uint32_t k = 0; k < 4u; ++k) {
		char ch                                                   = lay[order][k];
		f[(size_t)(2u * py + (k >> 1)) * sw + 2u * px + (k & 1u)] = c[ch == 'R'   ? 0
		                                                              : ch == 'G' ? 1
		                                                                          : 2];
	}
}

static void check_bayer(void)
{
	static const uint16_t col[4][3] = {
		{ 1023, 0, 0 }, { 0, 1023, 0 }, { 0, 0, 1023 }, { 512, 512, 512 }
	};
	const uint32_t ow = 8u, oh = 4u, sw = 2u * ow, sh = 2u * oh;
	const size_t   ys = 11u, uvo = 11u * 4u + 5u, uvs = 13u; /* odd strides + gap */
	for (int order = 0; order < 4; ++order) {
		uint16_t f[16 * 8];
		uint8_t  o[11 * 4 + 5 + 13 * 2 + 16];
		memset(o, 0xAA, sizeof(o));
		for (uint32_t by = 0; by < 2u; ++by)
			for (uint32_t bx = 0; bx < 4u; ++bx) {
				const uint16_t *c = col[(bx + 2u * by) & 3u];
				for (uint32_t k = 0; k < 4u; ++k)
					bayer_set(f, sw, 2u * bx + (k & 1u), 2u * by + (k >> 1), order, c);
			}
		alp_gst_bayer10_to_nv12(f, sw, sh, order, 256u, 256u, o, ys, uvo, uvs);
		for (uint32_t by = 0; by < 2u; ++by)
			for (uint32_t bx = 0; bx < 4u; ++bx) {
				const uint16_t *c = col[(bx + 2u * by) & 3u];
				double          r = c[0] / 1023.0, g = c[1] / 1023.0, b = c[2] / 1023.0;
				double          yy = 0.299 * r + 0.587 * g + 0.114 * b;
				int ey = (int)(16 + 219 * yy + 0.5), eu = (int)(128 + 224 * 0.564 * (b - yy) + 0.5);
				int ev = (int)(128 + 224 * 0.713 * (r - yy) + 0.5);
				for (uint32_t k = 0; k < 4u; ++k)
					ALP_ASSERT_TRUE(
					    abs((int)(o[(2u * by + (k >> 1)) * ys + 2u * bx + (k & 1u)]) - ey) <= 2);
				ALP_ASSERT_TRUE(abs((int)(o[uvo + by * uvs + 2u * bx]) - eu) <= 2);
				ALP_ASSERT_TRUE(abs((int)(o[uvo + by * uvs + 2u * bx + 1u]) - ev) <= 2);
			}
		for (uint32_t y = 0; y < 4u; ++y)
			for (size_t x = ow; x < ys; ++x)
				ALP_ASSERT_EQ_INT(o[y * ys + x], 0xAA); /* Y padding untouched */
		for (size_t x = ys * 4u; x < uvo; ++x)
			ALP_ASSERT_EQ_INT(o[x], 0xAA); /* gap untouched */
		for (uint32_t y = 0; y < 2u; ++y)
			for (size_t x = ow; x < uvs; ++x)
				ALP_ASSERT_EQ_INT(o[uvo + y * uvs + x], 0xAA); /* UV padding untouched */
	}
	/* gain: red 400 at 2.0 == red 800 at 1.0 */
	{
		uint16_t       a[16 * 8], b[16 * 8];
		uint8_t        oa[8 * 4 + 8 * 2], ob[8 * 4 + 8 * 2];
		const uint16_t c1[3] = { 400, 100, 50 }, c2[3] = { 800, 100, 50 };
		for (uint32_t y = 0; y < 4u; ++y)
			for (uint32_t x = 0; x < 8u; ++x) {
				bayer_set(a, 16u, x, y, ALP_GST_BAYER_RGGB, c1);
				bayer_set(b, 16u, x, y, ALP_GST_BAYER_RGGB, c2);
			}
		alp_gst_bayer10_to_nv12(
		    a, 16u, 8u, ALP_GST_BAYER_RGGB, alp_gst_gain_q8(2.0), 256u, oa, 8u, 32u, 8u);
		alp_gst_bayer10_to_nv12(b, 16u, 8u, ALP_GST_BAYER_RGGB, 256u, 256u, ob, 8u, 32u, 8u);
		ALP_ASSERT_EQ_INT(memcmp(oa, ob, sizeof(oa)), 0);
	}
	ALP_ASSERT_EQ_INT(alp_gst_bayer_parse("GRBG"), ALP_GST_BAYER_GRBG);
	ALP_ASSERT_EQ_INT(alp_gst_bayer_parse("gbrgx"), -1);
}

int main(void)
{
	size_t   cru_stride = W * 8u;
	uint8_t *cr10       = malloc(cru_stride * H);
	uint8_t  g8[W * H];
	uint16_t r10[W * H];
	pack_frame(cr10, cru_stride);
	for (uint32_t y = 0; y < H; ++y) {
		/* the two sizes the SDK backend produces from the same CR10 rows */
		cam_cr10_unpack_row(cr10 + y * cru_stride, W, g8 + y * W, 1u, 2u);
		cam_cr10_unpack_row(cr10 + y * cru_stride, W, (uint8_t *)(r10 + y * W), 2u, 0u);
	}

	/* GRAY8: stride 16 (13 rounded up to 4), canary in the padding */
	uint8_t o8[16 * H];
	memset(o8, 0xAA, sizeof(o8));
	alp_gst_gray8(g8, W, H, o8, 16u);
	for (uint32_t y = 0; y < H; ++y) {
		for (uint32_t x = 0; x < W; ++x)
			ALP_ASSERT_EQ_INT(o8[y * 16 + x], sample(x, y) >> 2);
		for (uint32_t x = W; x < 16u; ++x)
			ALP_ASSERT_EQ_INT(o8[y * 16 + x], 0xAA);
	}

	/* GRAY16_LE: full-range scaling, little-endian */
	uint8_t o16[32 * H];
	memset(o16, 0xAA, sizeof(o16));
	alp_gst_raw10_to_gray16(r10, W, H, o16, 32u);
	for (uint32_t y = 0; y < H; ++y) {
		for (uint32_t x = 0; x < W; ++x) {
			uint32_t v = sample(x, y), e = (v << 6) | (v >> 4);
			ALP_ASSERT_EQ_INT(o16[y * 32 + 2 * x], e & 0xFFu);
			ALP_ASSERT_EQ_INT(o16[y * 32 + 2 * x + 1], e >> 8);
		}
		for (uint32_t x = 2 * W; x < 32u; ++x)
			ALP_ASSERT_EQ_INT(o16[y * 32 + x], 0xAA);
	}
	{ /* range ends */
		uint16_t ends[2] = { 0u, 0x3FFu };
		uint8_t  o[4];
		alp_gst_raw10_to_gray16(ends, 2u, 1u, o, 4u);
		ALP_ASSERT_EQ_INT(o[0] | (o[1] << 8), 0x0000);
		ALP_ASSERT_EQ_INT(o[2] | (o[3] << 8), 0xFFFF);
	}

	/* NV12 for 12x4 (even): Y stride 12, UV plane at 48, stride 12; UV all 128 */
	uint8_t onv[12 * 4 + 12 * 2];
	memset(onv, 0xAA, sizeof(onv));
	uint8_t tight[12 * H];
	for (uint32_t y = 0; y < H; ++y)
		memcpy(tight + y * 12, g8 + y * W, 12u);
	alp_gst_gray8_to_nv12(tight, 12u, H, onv, 12u, 48u, 12u);
	for (uint32_t i = 0; i < 48u; ++i)
		ALP_ASSERT_EQ_INT(onv[i], tight[i]);
	for (uint32_t i = 48u; i < sizeof(onv); ++i)
		ALP_ASSERT_EQ_INT(onv[i], 128);

	/* NV12 with padded strides: w=14 -> stride 16, UV plane beyond w*h (offset 80) */
	{
		uint8_t src[14 * 4], o[16 * 4 + 16 * 2 + 16];
		for (uint32_t i = 0; i < sizeof(src); ++i)
			src[i] = (uint8_t)(i + 1u);
		memset(o, 0xAA, sizeof(o));
		alp_gst_gray8_to_nv12(src, 14u, 4u, o, 16u, 80u, 16u);
		for (uint32_t y = 0; y < 4u; ++y) {
			for (uint32_t x = 0; x < 14u; ++x)
				ALP_ASSERT_EQ_INT(o[y * 16 + x], src[y * 14 + x]);
			for (uint32_t x = 14u; x < 16u; ++x)
				ALP_ASSERT_EQ_INT(o[y * 16 + x], 0xAA);
		}
		for (uint32_t i = 64u; i < 80u; ++i) /* gap before the UV plane */
			ALP_ASSERT_EQ_INT(o[i], 0xAA);
		for (uint32_t y = 0; y < 2u; ++y) {
			for (uint32_t x = 0; x < 14u; ++x)
				ALP_ASSERT_EQ_INT(o[80 + y * 16 + x], 128);
			for (uint32_t x = 14u; x < 16u; ++x)
				ALP_ASSERT_EQ_INT(o[80 + y * 16 + x], 0xAA);
		}
	}

	check_bayer();
	free(cr10);
	printf("gst_alpcamerasrc_convert: GRAY8/GRAY16_LE/NV12/Bayer ok, neon=%d\n",
	       CAM_CR10_HAVE_NEON);
	ALP_TEST_SUMMARY();
}
