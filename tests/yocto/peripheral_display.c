/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #1143's Yocto display backend
 * (src/backends/display/yocto_drv.c).
 *
 * The real y_open()/y_close() drive DRM/KMS ioctls against a real
 * /dev/dri/card* node, which does not exist in this build
 * environment (BENCH-UNVERIFIED, per that file's own header) -- so
 * this test targets the PURE logic factored out of the ops:
 * _blit_copy() and _clear_fill() operate on a plain malloc()'d buffer
 * standing in for the real mmap()ed dumb-buffer pointer, exercising
 * exactly the range-check + pitch-aware copy/fill code the real ops
 * call, with no ioctl/mmap involved.
 *
 * This file #includes the real backend .c file directly (same
 * technique as tests/yocto/peripheral_usb.c) to reach its file-local
 * _blit_copy()/_clear_fill()/_pick_crtc().
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_display
 *   ctest --test-dir build -R alp_test_peripheral_display
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "test_assert.h"

#include "../../src/backends/display/yocto_drv.c"

/* A 4x4 ARGB8888 framebuffer with a deliberately padded pitch (24
 * bytes/row instead of the tight 16) -- proves _blit_copy()/
 * _clear_fill() honour fb_pitch instead of assuming w*4. */
#define FB_W     4u
#define FB_H     4u
#define FB_PITCH 24u

static void test_blit_copy_rejects_zero_area(void)
{
	uint8_t fb[FB_PITCH * FB_H] = { 0 };
	uint8_t px[4]               = { 0x11, 0x22, 0x33, 0x44 };

	ALP_ASSERT_EQ_INT(_blit_copy(fb, FB_PITCH, FB_W, FB_H, 0, 0, 0, 1, px), ALP_ERR_INVAL);
	ALP_ASSERT_EQ_INT(_blit_copy(fb, FB_PITCH, FB_W, FB_H, 0, 0, 1, 0, px), ALP_ERR_INVAL);
}

static void test_blit_copy_rejects_out_of_range(void)
{
	uint8_t fb[FB_PITCH * FB_H] = { 0 };
	uint8_t px[4 * 4]           = { 0 };

	/* x + w = 3 + 2 = 5 > FB_W (4) */
	ALP_ASSERT_EQ_INT(_blit_copy(fb, FB_PITCH, FB_W, FB_H, 3, 0, 2, 1, px), ALP_ERR_OUT_OF_RANGE);
	/* y + h = 3 + 2 = 5 > FB_H (4) */
	ALP_ASSERT_EQ_INT(_blit_copy(fb, FB_PITCH, FB_W, FB_H, 0, 3, 1, 2, px), ALP_ERR_OUT_OF_RANGE);
}

static void test_blit_copy_rejects_null(void)
{
	uint8_t fb[FB_PITCH * FB_H] = { 0 };
	uint8_t px[4]               = { 0 };

	ALP_ASSERT_EQ_INT(_blit_copy(NULL, FB_PITCH, FB_W, FB_H, 0, 0, 1, 1, px), ALP_ERR_INVAL);
	ALP_ASSERT_EQ_INT(_blit_copy(fb, FB_PITCH, FB_W, FB_H, 0, 0, 1, 1, NULL), ALP_ERR_INVAL);
}

/* Load-bearing test: a 2x2 rect at (1,1) must land at the RIGHT
 * offset in a PADDED-pitch buffer -- a version that used fb_w*4
 * instead of fb_pitch as the row stride (the bug this test exists to
 * catch) would write into the wrong row entirely. */
static void test_blit_copy_honours_pitch(void)
{
	uint8_t fb[FB_PITCH * FB_H];
	memset(fb, 0xAA, sizeof(fb)); /* sentinel -- untouched bytes must stay this */

	/* 2x2 source, distinct bytes per pixel so a transposition/offset
	 * bug is visible. */
	uint8_t src[2 * 2 * 4] = {
		0x01, 0x02, 0x03, 0x04, /* (0,0) */
		0x05, 0x06, 0x07, 0x08, /* (1,0) */
		0x09, 0x0A, 0x0B, 0x0C, /* (0,1) */
		0x0D, 0x0E, 0x0F, 0x10, /* (1,1) */
	};

	ALP_ASSERT_EQ_INT(_blit_copy(fb, FB_PITCH, FB_W, FB_H, 1, 1, 2, 2, src), ALP_OK);

	/* Row 1 (y=1), pixel x=1 starts at byte offset 1*FB_PITCH + 1*4. */
	ALP_ASSERT_TRUE(memcmp(fb + 1 * FB_PITCH + 1 * 4, &src[0], 4) == 0);
	ALP_ASSERT_TRUE(memcmp(fb + 1 * FB_PITCH + 2 * 4, &src[4], 4) == 0);
	/* Row 2 (y=2), pixel x=1 and x=2. */
	ALP_ASSERT_TRUE(memcmp(fb + 2 * FB_PITCH + 1 * 4, &src[8], 4) == 0);
	ALP_ASSERT_TRUE(memcmp(fb + 2 * FB_PITCH + 2 * 4, &src[12], 4) == 0);

	/* The pitch padding past FB_W*4 on row 1 must be untouched. */
	ALP_ASSERT_EQ_INT(fb[1 * FB_PITCH + FB_W * 4], 0xAA);
}

static void test_clear_fill_zeroes_every_row_honouring_pitch(void)
{
	uint8_t fb[FB_PITCH * FB_H];
	memset(fb, 0xAA, sizeof(fb));

	ALP_ASSERT_EQ_INT(_clear_fill(fb, FB_PITCH, FB_W, FB_H), ALP_OK);

	for (uint32_t row = 0; row < FB_H; ++row) {
		for (uint32_t b = 0; b < FB_W * 4u; ++b) {
			ALP_ASSERT_EQ_INT(fb[row * FB_PITCH + b], 0);
		}
		/* Padding past the visible row width is NOT this function's
		 * business -- confirm it is untouched, not just happen to be
		 * zero from the sentinel fill. */
		for (uint32_t b = FB_W * 4u; b < FB_PITCH; ++b) {
			ALP_ASSERT_EQ_INT(fb[row * FB_PITCH + b], 0xAA);
		}
	}
}

static void test_clear_fill_rejects_null(void)
{
	ALP_ASSERT_EQ_INT(_clear_fill(NULL, FB_PITCH, FB_W, FB_H), ALP_ERR_INVAL);
}

/* _pick_crtc(): the possible_crtcs bitmask indexes into the crtc_ids
 * array; a bit for an index past `count` must not select anything,
 * and index-32+ must not shift-UB. */
static void test_pick_crtc_selects_first_allowed(void)
{
	uint32_t crtc_ids[3] = { 10, 20, 30 };
	uint32_t out         = 0;

	/* Only bit 1 (crtc_ids[1] == 20) is allowed. */
	ALP_ASSERT_EQ_INT(_pick_crtc(crtc_ids, 3, 0x2u, &out), ALP_OK);
	ALP_ASSERT_EQ_INT((int)out, 20);
}

static void test_pick_crtc_no_match_is_not_ready(void)
{
	uint32_t crtc_ids[2] = { 10, 20 };
	uint32_t out         = 999;

	ALP_ASSERT_EQ_INT(_pick_crtc(crtc_ids, 2, 0x0u, &out), ALP_ERR_NOT_READY);
	ALP_ASSERT_EQ_INT((int)out, 999); /* untouched on failure */
}

/* The modeset gate is the whole point of this backend's safety posture,
 * and until this case existed nothing exercised it: config_defaults.c
 * pins the MACRO's value, not the backend's behaviour, so deleting
 * `if (!cfg->allow_modeset)` from y_open() left every gate green.
 *
 * y_open() refuses before it opens anything, so this needs no
 * /dev/dri/card* node and no DRM master -- a default-config call must
 * come back ALP_ERR_INVAL on any host. */
static void test_open_refuses_without_allow_modeset(void)
{
	alp_display_config_t        cfg   = ALP_DISPLAY_CONFIG_DEFAULT(0);
	alp_display_backend_state_t state = { 0 };
	alp_capabilities_t          caps  = { 0 };

	ALP_ASSERT_EQ_INT(y_open(&cfg, &state, &caps), ALP_ERR_INVAL);
	/* Refused before touching the device: nothing was published. */
	ALP_ASSERT_TRUE(state.be_data == NULL);
}

int main(void)
{
	test_open_refuses_without_allow_modeset();
	test_blit_copy_rejects_zero_area();
	test_blit_copy_rejects_out_of_range();
	test_blit_copy_rejects_null();
	test_blit_copy_honours_pitch();
	test_clear_fill_zeroes_every_row_honouring_pitch();
	test_clear_fill_rejects_null();
	test_pick_crtc_selects_first_allowed();
	test_pick_crtc_no_match_is_not_ready();

	ALP_TEST_SUMMARY();
}
