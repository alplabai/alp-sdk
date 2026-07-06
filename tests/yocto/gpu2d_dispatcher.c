/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plain-CMake tests for the <alp/gpu2d.h> dispatcher on ALP_OS=yocto
 * (src/gpu2d_dispatch.c routed to the portable sw_fallback backend --
 * the only GPU2D backend on a Linux build until a PXP/libg2d silicon
 * backend lands, see ADR 0008 / issue #24).
 *
 * The Zephyr side has parallel (and deeper, exact-pixel) coverage in
 * tests/unit/gpu2d_registry; this suite proves the class actually
 * dispatches on the plain-CMake Yocto build -- i.e. that Linux apps
 * get the REAL CPU fill/blit/blend, not the old NOSUPPORT stub --
 * plus the dispatcher edges that guard it (pool exhaustion, surface
 * validation).
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_gpu2d_dispatcher
 *   ctest --test-dir build -R alp_test_gpu2d_dispatcher
 */

#include <stdint.h>

#include "alp/cap_instance.h"
#include "alp/gpu2d.h"
#include "alp/peripheral.h"

#include "test_assert.h"

#define W 4
#define H 4

/* W x H ARGB8888 framebuffer + one guard word so an over-write past
 * the surface is caught. */
static uint32_t fb[W * H + 1];

static alp_gpu2d_surface_t argb_surface(void)
{
	alp_gpu2d_surface_t s = {
		.base         = fb,
		.width        = W,
		.height       = H,
		.stride_bytes = W * sizeof(uint32_t),
		.format       = ALP_GPU2D_FMT_ARGB8888,
	};
	return s;
}

static void fb_clear(uint32_t val)
{
	for (int i = 0; i < W * H; ++i) {
		fb[i] = val;
	}
	fb[W * H] = 0xDEADBEEFu; /* guard */
}

static void test_open_succeeds_with_sw_fallback(void)
{
	alp_gpu2d_t *g = alp_gpu2d_open();
	ALP_ASSERT_TRUE(g != NULL);
	/* CPU path: caps present, no DMA flag. */
	const alp_capabilities_t *caps = alp_gpu2d_capabilities(g);
	ALP_ASSERT_TRUE(caps != NULL);
	ALP_ASSERT_TRUE(!alp_capabilities_has(caps, ALP_INSTANCE_CAP_DMA));
	alp_gpu2d_close(g);
}

static void test_pool_exhaustion_then_reopen(void)
{
	/* The handle pool defaults to ONE: a second concurrent open
	 * fails with NOMEM; after close the slot is reusable. */
	alp_gpu2d_t *g = alp_gpu2d_open();
	ALP_ASSERT_TRUE(g != NULL);
	alp_gpu2d_t *g2 = alp_gpu2d_open();
	ALP_ASSERT_NULL(g2);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_NOMEM);
	alp_gpu2d_close(g);
	g = alp_gpu2d_open();
	ALP_ASSERT_TRUE(g != NULL);
	alp_gpu2d_close(g);
}

static void test_fill_rect_exact_pixels_and_clip(void)
{
	alp_gpu2d_t *g = alp_gpu2d_open();
	ALP_ASSERT_TRUE(g != NULL);
	fb_clear(0x00000000u);
	alp_gpu2d_surface_t dst = argb_surface();

	/* 2x2 red rect at (1,1): ARGB8888 in-memory word 0xFFFF0000. */
	ALP_ASSERT_EQ_INT(alp_gpu2d_fill_rect(g, &dst, 1u, 1u, 2u, 2u, 0xFFFF0000u), ALP_OK);
	for (int y = 0; y < H; ++y) {
		for (int x = 0; x < W; ++x) {
			uint32_t want = (x >= 1 && x <= 2 && y >= 1 && y <= 2) ? 0xFFFF0000u : 0u;
			ALP_ASSERT_EQ_INT(fb[y * W + x], want);
		}
	}

	/* Over-large rect clips to the surface -- guard stays intact. */
	ALP_ASSERT_EQ_INT(alp_gpu2d_fill_rect(g, &dst, 2u, 2u, 100u, 100u, 0xFF00FF00u), ALP_OK);
	ALP_ASSERT_EQ_INT(fb[3 * W + 3], 0xFF00FF00u);
	ALP_ASSERT_EQ_INT(fb[W * H], 0xDEADBEEFu);
	alp_gpu2d_close(g);
}

static void test_undersized_stride_rejected(void)
{
	/* Dispatcher surface validation: rows must fit their stride. */
	alp_gpu2d_t *g = alp_gpu2d_open();
	ALP_ASSERT_TRUE(g != NULL);
	alp_gpu2d_surface_t bad = argb_surface();
	bad.stride_bytes        = 8; /* 4 px ARGB8888 rows need 16 B */
	ALP_ASSERT_EQ_INT(alp_gpu2d_fill_rect(g, &bad, 0u, 0u, 2u, 2u, 0xFF112233u), ALP_ERR_INVAL);
	alp_gpu2d_close(g);
}

static void test_blend_src_over_opaque_and_transparent(void)
{
	static uint32_t     src_buf[W * H];
	alp_gpu2d_t        *g = alp_gpu2d_open();
	alp_gpu2d_surface_t src;
	alp_gpu2d_surface_t dst = argb_surface();

	ALP_ASSERT_TRUE(g != NULL);
	src      = argb_surface();
	src.base = src_buf;

	/* Opaque src wins fully. */
	for (int i = 0; i < W * H; ++i) {
		src_buf[i] = 0xFF112233u;
	}
	fb_clear(0xFF445566u);
	ALP_ASSERT_EQ_INT(
	    alp_gpu2d_blend(g, &src, 0u, 0u, &dst, 0u, 0u, 1u, 1u, ALP_GPU2D_BLEND_SRC_OVER), ALP_OK);
	ALP_ASSERT_EQ_INT(fb[0], 0xFF112233u);

	/* Fully transparent src leaves dst untouched. */
	for (int i = 0; i < W * H; ++i) {
		src_buf[i] = 0x00112233u;
	}
	fb_clear(0xFF445566u);
	ALP_ASSERT_EQ_INT(
	    alp_gpu2d_blend(g, &src, 0u, 0u, &dst, 0u, 0u, 1u, 1u, ALP_GPU2D_BLEND_SRC_OVER), ALP_OK);
	ALP_ASSERT_EQ_INT(fb[0], 0xFF445566u);
	alp_gpu2d_close(g);
}

int main(void)
{
	test_open_succeeds_with_sw_fallback();
	test_pool_exhaustion_then_reopen();
	test_fill_rect_exact_pixels_and_clip();
	test_undersized_stride_rejected();
	test_blend_src_over_opaque_and_transparent();
	ALP_TEST_SUMMARY();
}
