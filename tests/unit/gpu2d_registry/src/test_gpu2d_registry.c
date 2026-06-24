/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the GPU2D class: the registry / dispatcher edges
 * AND the real software-fallback pixel ops.
 *
 * Backends visible on this native_sim build:
 *   sw_fallback   (priority 0, "*" wildcard, vendor "sw")
 *
 * sw_fallback is a REAL backend: open() returns ALP_OK and
 * fill_rect / blit / blend do actual CPU pixel work over caller
 * memory.  The Alif D/AVE 2D backend (alif_dave2d.c) is gated on
 * the proprietary Dave2D pack and cannot build on native_sim, so it
 * never registers here -- the wildcard sw_fallback is the only
 * backend on this build.
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("gpu2d", ALP_SOC_REF_STR)`
 * exercises the same selector code path real customer builds hit.
 * Even on an E7 silicon_ref the dave2d backend is absent on
 * native_sim, so selection lands on the wildcard sw_fallback.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/gpu2d.h>
#include <alp/peripheral.h>

#include "../../../../src/backends/gpu2d/gpu2d_ops.h"

ZTEST_SUITE(alp_gpu2d_registry, NULL, NULL, NULL, NULL, NULL);

/* Scratch backing buffer + valid surfaces for the dispatcher edge
 * tests.  Both src and dst point at the same scratch -- these tests
 * only exercise the dispatcher's pointer/dim/format-range and mode
 * validation, not the pixel content. */
static uint8_t                   _scratch[16];
static const alp_gpu2d_surface_t _valid_src = {
	.base         = _scratch,
	.width        = 4,
	.height       = 4,
	.stride_bytes = 4,
	.format       = ALP_GPU2D_FMT_RGBA8888,
};
static const alp_gpu2d_surface_t _valid_dst = {
	.base         = _scratch,
	.width        = 4,
	.height       = 4,
	.stride_bytes = 4,
	.format       = ALP_GPU2D_FMT_RGBA8888,
};

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_gpu2d_registry, test_sw_fallback_picked_for_alif_e7)
{
	/* The wildcard sw_fallback is the only GPU2D backend on this
     * native_sim build (the dave2d backend needs the Dave2D pack);
     * any silicon_ref resolves to it. */
	const alp_backend_t *be = alp_backend_select("gpu2d", "alif:ensemble:e7");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "sw"), 0);
	zassert_equal(be->priority, 0);
}

ZTEST(alp_gpu2d_registry, test_sw_fallback_picked_for_unknown_silicon)
{
	const alp_backend_t *be = alp_backend_select("gpu2d", "fictional:soc:zz");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "sw"), 0);
	zassert_equal(be->priority, 0);
}

ZTEST(alp_gpu2d_registry, test_select_returns_null_for_null_class)
{
	zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_gpu2d_registry, test_select_returns_null_for_null_silicon_ref)
{
	/* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
	zassert_is_null(alp_backend_select("gpu2d", NULL));
}

ZTEST(alp_gpu2d_registry, test_backend_count_for_gpu2d)
{
	/* Only sw_fallback registers on native_sim. */
	zassert_equal(alp_backend_count("gpu2d"), 1u);
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_gpu2d_registry, test_gpu2d_capabilities_returns_null_for_null_handle)
{
	zassert_is_null(alp_gpu2d_capabilities(NULL));
}

ZTEST(alp_gpu2d_registry, test_open_succeeds_with_sw_fallback)
{
	/* sw_fallback is a real backend: open() succeeds portably. */
	alp_gpu2d_t *g = alp_gpu2d_open();
	zassert_not_null(g);
	/* CPU path advertises no DMA cap. */
	const alp_capabilities_t *caps = alp_gpu2d_capabilities(g);
	zassert_not_null(caps);
	zassert_false(alp_capabilities_has(caps, ALP_INSTANCE_CAP_DMA));
	alp_gpu2d_close(g);
}

ZTEST(alp_gpu2d_registry, test_ops_null_handle_not_ready)
{
	/* The dispatcher's NULL-handle guard surfaces NOT_READY before
     * any backend op or surface validation. */
	zassert_equal(alp_gpu2d_fill_rect(NULL, &_valid_dst, 0u, 0u, 4u, 4u, 0xffu), ALP_ERR_NOT_READY);
	zassert_equal(
	    alp_gpu2d_blend(
	        NULL, &_valid_src, 0u, 0u, &_valid_dst, 0u, 0u, 4u, 4u, (alp_gpu2d_blend_mode_t)0),
	    ALP_ERR_NOT_READY);
}

ZTEST(alp_gpu2d_registry, test_invalid_surface_rejected)
{
	/* Dispatcher surface validation still fires with a live handle. */
	alp_gpu2d_t *g = alp_gpu2d_open();
	zassert_not_null(g);
	alp_gpu2d_surface_t bad = _valid_dst;
	bad.base                = NULL;
	zassert_equal(alp_gpu2d_fill_rect(g, &bad, 0u, 0u, 2u, 2u, 0xff112233u), ALP_ERR_INVAL);
	bad        = _valid_dst;
	bad.format = (alp_gpu2d_format_t)99;
	zassert_equal(alp_gpu2d_fill_rect(g, &bad, 0u, 0u, 2u, 2u, 0xff112233u), ALP_ERR_INVAL);
	alp_gpu2d_close(g);
}

ZTEST(alp_gpu2d_registry, test_blend_mode_range_rejected)
{
	alp_gpu2d_t *g = alp_gpu2d_open();
	zassert_not_null(g);
	zassert_equal(
	    alp_gpu2d_blend(
	        g, &_valid_src, 0u, 0u, &_valid_dst, 0u, 0u, 4u, 4u, (alp_gpu2d_blend_mode_t)42),
	    ALP_ERR_INVAL);
	alp_gpu2d_close(g);
}

/* ---------- REAL pixel-op tests (sw_fallback) ----------------------- */

/* 4x4 ARGB8888 scratch surfaces with a guard sentinel after each, so
 * an over-write past the surface is caught. */
#define W 4
#define H 4
static uint32_t _fb_a[W * H + 1];
static uint32_t _fb_b[W * H + 1];

static alp_gpu2d_surface_t _argb(uint32_t *buf)
{
	alp_gpu2d_surface_t s = {
		.base         = buf,
		.width        = W,
		.height       = H,
		.stride_bytes = W * sizeof(uint32_t),
		.format       = ALP_GPU2D_FMT_ARGB8888,
	};
	return s;
}

static void _clear(uint32_t *buf, uint32_t val)
{
	for (int i = 0; i < W * H; ++i) {
		buf[i] = val;
	}
	buf[W * H] = 0xDEADBEEFu; /* guard sentinel */
}

ZTEST(alp_gpu2d_registry, test_fill_rect_argb8888_exact_and_clipped)
{
	alp_gpu2d_t *g = alp_gpu2d_open();
	zassert_not_null(g);
	_clear(_fb_a, 0x00000000u);
	alp_gpu2d_surface_t dst = _argb(_fb_a);

	/* Fill a 2x2 rect at (1,1) red.  Stored ARGB8888 little-endian
     * 0xAARRGGBB == in-memory uint32 0xFFFF0000. */
	zassert_equal(alp_gpu2d_fill_rect(g, &dst, 1u, 1u, 2u, 2u, 0xFFFF0000u), ALP_OK);

	for (int y = 0; y < H; ++y) {
		for (int x = 0; x < W; ++x) {
			uint32_t want = (x >= 1 && x <= 2 && y >= 1 && y <= 2) ? 0xFFFF0000u : 0x00000000u;
			zassert_equal(_fb_a[y * W + x], want, "pixel (%d,%d)", x, y);
		}
	}

	/* Over-large rect clips to the surface, never the guard. */
	zassert_equal(alp_gpu2d_fill_rect(g, &dst, 2u, 2u, 100u, 100u, 0xFF00FF00u), ALP_OK);
	zassert_equal(_fb_a[3 * W + 3], 0xFF00FF00u);
	zassert_equal(_fb_a[W * H], 0xDEADBEEFu, "guard overwritten -- clip failed");

	/* Fully off-surface origin is a no-op (still ALP_OK). */
	zassert_equal(alp_gpu2d_fill_rect(g, &dst, 10u, 10u, 4u, 4u, 0xFFFFFFFFu), ALP_OK);
	alp_gpu2d_close(g);
}

ZTEST(alp_gpu2d_registry, test_blit_argb_to_argb_exact)
{
	alp_gpu2d_t *g = alp_gpu2d_open();
	zassert_not_null(g);
	_clear(_fb_a, 0x00000000u);
	_clear(_fb_b, 0xFFFFFFFFu);
	alp_gpu2d_surface_t src = _argb(_fb_a);
	alp_gpu2d_surface_t dst = _argb(_fb_b);

	/* Paint a single blue pixel into src at (0,0) and blit a 1x1 to
     * dst (2,3). */
	_fb_a[0] = 0xFF0000FFu;
	zassert_equal(alp_gpu2d_blit(g, &src, 0u, 0u, &dst, 2u, 3u, 1u, 1u), ALP_OK);
	zassert_equal(_fb_b[3 * W + 2], 0xFF0000FFu);
	/* Neighbours untouched. */
	zassert_equal(_fb_b[3 * W + 1], 0xFFFFFFFFu);
	alp_gpu2d_close(g);
}

ZTEST(alp_gpu2d_registry, test_blit_argb_to_rgb565_roundtrip)
{
	alp_gpu2d_t *g = alp_gpu2d_open();
	zassert_not_null(g);
	uint16_t rgb565[W * H];
	memset(rgb565, 0, sizeof(rgb565));
	_clear(_fb_a, 0u);
	_fb_a[0]                = 0xFFFF0000u; /* pure red */
	alp_gpu2d_surface_t src = _argb(_fb_a);
	alp_gpu2d_surface_t dst = {
		.base         = rgb565,
		.width        = W,
		.height       = H,
		.stride_bytes = W * sizeof(uint16_t),
		.format       = ALP_GPU2D_FMT_RGB565,
	};
	zassert_equal(alp_gpu2d_blit(g, &src, 0u, 0u, &dst, 0u, 0u, 1u, 1u), ALP_OK);
	/* Pure red ARGB8888 -> RGB565 = R:0x1F G:0 B:0 = 0xF800. */
	zassert_equal(rgb565[0], 0xF800u, "got 0x%04x", rgb565[0]);
	alp_gpu2d_close(g);
}

ZTEST(alp_gpu2d_registry, test_fill_rect_honours_padded_stride)
{
	/* Surface with stride_bytes > width*bpp: (W+2) px allocated per
     * row but only W px wide.  A row-stepping bug (stepping by
     * width*bpp instead of stride_bytes) or an off-by-one in _pix()
     * would corrupt the inter-row padding or land pixels at the wrong
     * offset; a tight-stride surface cannot catch either. */
#define PAD_ROW (W + 2)
	static uint32_t fb[PAD_ROW * H + 1];
	for (int i = 0; i < PAD_ROW * H; ++i) {
		fb[i] = 0xCAFEF00Du; /* fill EVERYTHING incl. padding */
	}
	fb[PAD_ROW * H] = 0xDEADBEEFu; /* guard */

	alp_gpu2d_surface_t dst = {
		.base         = fb,
		.width        = W,
		.height       = H,
		.stride_bytes = PAD_ROW * sizeof(uint32_t),
		.format       = ALP_GPU2D_FMT_ARGB8888,
	};

	alp_gpu2d_t *g = alp_gpu2d_open();
	zassert_not_null(g);

	/* Fill the whole logical WxH area green. */
	zassert_equal(alp_gpu2d_fill_rect(g, &dst, 0u, 0u, W, H, 0xFF00FF00u), ALP_OK);

	for (int y = 0; y < H; ++y) {
		/* The W logical pixels at base + y*stride land green. */
		for (int x = 0; x < W; ++x) {
			zassert_equal(fb[y * PAD_ROW + x], 0xFF00FF00u, "pixel (%d,%d)", x, y);
		}
		/* The 2 padding pixels after each row stay untouched -- proves
         * the op stepped by stride_bytes, not width*bpp. */
		zassert_equal(fb[y * PAD_ROW + W], 0xCAFEF00Du, "row %d pad[0] clobbered", y);
		zassert_equal(fb[y * PAD_ROW + W + 1], 0xCAFEF00Du, "row %d pad[1] clobbered", y);
	}
	zassert_equal(fb[PAD_ROW * H], 0xDEADBEEFu, "guard overwritten");
	alp_gpu2d_close(g);
#undef PAD_ROW
}

ZTEST(alp_gpu2d_registry, test_fill_rect_huge_wh_does_not_overflow_clip)
{
	/* Regression for the _clip() integer-overflow: a caller passing
     * w/h = UINT32_MAX as a "fill everything" sentinel must clip to
     * the surface, never wrap x + *w below s->width and write past
     * the buffer. */
	alp_gpu2d_t *g = alp_gpu2d_open();
	zassert_not_null(g);
	_clear(_fb_a, 0x00000000u);
	alp_gpu2d_surface_t dst = _argb(_fb_a);

	zassert_equal(alp_gpu2d_fill_rect(g, &dst, 1u, 1u, UINT32_MAX, UINT32_MAX, 0xFF00FF00u),
	              ALP_OK);
	/* In-bounds corner filled, guard intact (no 4 GB wrap-write). */
	zassert_equal(_fb_a[3 * W + 3], 0xFF00FF00u);
	zassert_equal(_fb_a[W * H], 0xDEADBEEFu, "guard overwritten -- overflow clip failed");
	alp_gpu2d_close(g);
}

ZTEST(alp_gpu2d_registry, test_blend_replace_and_src_over_and_additive_and_multiply)
{
	alp_gpu2d_t *g = alp_gpu2d_open();
	zassert_not_null(g);
	alp_gpu2d_surface_t src = _argb(_fb_a);
	alp_gpu2d_surface_t dst = _argb(_fb_b);

	/* REPLACE: dst becomes src exactly. */
	_clear(_fb_a, 0x11223344u);
	_clear(_fb_b, 0xFFFFFFFFu);
	zassert_equal(alp_gpu2d_blend(g, &src, 0u, 0u, &dst, 0u, 0u, 1u, 1u, ALP_GPU2D_BLEND_REPLACE),
	              ALP_OK);
	zassert_equal(_fb_b[0], 0x11223344u);

	/* SRC_OVER with opaque src (alpha 0xFF) == src wins fully. */
	_clear(_fb_a, 0xFF112233u);
	_clear(_fb_b, 0xFF445566u);
	zassert_equal(alp_gpu2d_blend(g, &src, 0u, 0u, &dst, 0u, 0u, 1u, 1u, ALP_GPU2D_BLEND_SRC_OVER),
	              ALP_OK);
	zassert_equal(_fb_b[0], 0xFF112233u);

	/* SRC_OVER with fully transparent src (alpha 0) == dst unchanged. */
	_clear(_fb_a, 0x00112233u);
	_clear(_fb_b, 0xFF445566u);
	zassert_equal(alp_gpu2d_blend(g, &src, 0u, 0u, &dst, 0u, 0u, 1u, 1u, ALP_GPU2D_BLEND_SRC_OVER),
	              ALP_OK);
	zassert_equal(_fb_b[0], 0xFF445566u);

	/* SRC_OVER half-alpha (straight-alpha): src A=0x80,R=0xFF over
     * dst A=0xFF,R=0x00.  ia = 255-128 = 127.
     * out R = (0xFF*128 + 0x00*127 + 127)/255 = 32767/255 = 0x80.
     * out A = 0x80 + (0xFF*127 + 127)/255 = 128 + 127 = 0xFF. */
	_clear(_fb_a, 0x80FF0000u);
	_clear(_fb_b, 0xFF000000u);
	zassert_equal(alp_gpu2d_blend(g, &src, 0u, 0u, &dst, 0u, 0u, 1u, 1u, ALP_GPU2D_BLEND_SRC_OVER),
	              ALP_OK);
	zassert_equal((_fb_b[0] >> 16) & 0xFFu, 0x80u, "R");
	zassert_equal((_fb_b[0] >> 24) & 0xFFu, 0xFFu, "A");

	/* ADDITIVE saturates: 0x80 + 0xA0 = 0x120 -> 0xFF per channel. */
	_clear(_fb_a, 0x80808080u);
	_clear(_fb_b, 0xA0A0A0A0u);
	zassert_equal(alp_gpu2d_blend(g, &src, 0u, 0u, &dst, 0u, 0u, 1u, 1u, ALP_GPU2D_BLEND_ADDITIVE),
	              ALP_OK);
	zassert_equal(_fb_b[0], 0xFFFFFFFFu);

	/* MULTIPLY: 0xFF * 0x80 / 255 = 0x80; 0x00 * x = 0x00. */
	_clear(_fb_a, 0xFFFF0000u);
	_clear(_fb_b, 0x80808080u);
	zassert_equal(alp_gpu2d_blend(g, &src, 0u, 0u, &dst, 0u, 0u, 1u, 1u, ALP_GPU2D_BLEND_MULTIPLY),
	              ALP_OK);
	/* A=0xFF*0x80=0x80, R=0xFF*0x80=0x80, G=0x00, B=0x00. */
	zassert_equal(_fb_b[0], 0x80800000u, "got 0x%08x", _fb_b[0]);

	alp_gpu2d_close(g);
}
