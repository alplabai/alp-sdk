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

	free(cr10);
	printf("gst_alpcamerasrc_convert: GRAY8/GRAY16_LE/NV12 ok, neon=%d\n", CAM_CR10_HAVE_NEON);
	ALP_TEST_SUMMARY();
}
