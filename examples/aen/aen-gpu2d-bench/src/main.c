/*
 * Copyright (c) 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * On-silicon <alp/gpu2d.h> validation for the E1M-AEN (Alif Ensemble E8,
 * M55-HE): fill_rect / blit / blend / close through whichever backend the
 * build selects, each checked against the pixel value the software fallback's
 * formulas give (src/backends/gpu2d/sw_fallback.c). ARGB8888 packs
 * little-endian as B,G,R,A, so a uint32 read of a pixel == 0xAARRGGBB.
 *
 * Default build: the priority-0 software fallback, the same CPU path every
 * no-2D-engine SoM uses. With the D/AVE 2D engine (#916):
 *
 *   west update --group-filter +vendor-sdks     # fetches alif_dave2d-driver
 *   west build ... -- -DEXTRA_CONF_FILE=overlay-dave2d.conf \
 *                     -DEXTRA_DTC_OVERLAY_FILE=dave2d.overlay
 *
 * The surfaces sit in the global SRAM0 bank because the engine is an AXI
 * master that cannot reach core-local DTCM. Blended channels may differ by 1
 * from the software values (<alp/gpu2d.h>), so SRC_OVER is checked to +-1.
 *
 * Console is the RAM buffer 'ram_console_buf' (see prj.conf).
 * BENCH-VALIDATION app -- not a customer teaching example.
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/linker/devicetree_regions.h>

#include <alp/gpu2d.h>
#include <alp/peripheral.h>

#if defined(CONFIG_ALP_SDK_GPU2D_ALIF_DAVE2D)
#define BACKEND "D/AVE 2D engine"
#else
#define BACKEND "sw_fallback"
#endif

/* Small ARGB8888 work surfaces.  uint32 elements so a pixel read == 0xAARRGGBB
 * (little-endian M55), giving exact-value checks with no byte juggling. */
#define DIM 8U
/* In the global SRAM0 bank, not core-local DTCM: the D/AVE 2D engine is an
 * AXI master and reads/writes these at the CPU's address. */
#if DT_NODE_EXISTS(DT_NODELABEL(sram0))
#define SURF_SECTION Z_GENERIC_SECTION(LINKER_DT_NODE_REGION_NAME(DT_NODELABEL(sram0)))
#else
#define SURF_SECTION
#endif
static uint32_t SURF_SECTION __aligned(32) dst_buf[DIM * DIM];
static uint32_t SURF_SECTION __aligned(32) src_buf[DIM * DIM];

static alp_gpu2d_surface_t dst = {
	.base         = dst_buf,
	.width        = DIM,
	.height       = DIM,
	.stride_bytes = DIM * 4U,
	.format       = ALP_GPU2D_FMT_ARGB8888,
};
static alp_gpu2d_surface_t src = {
	.base         = src_buf,
	.width        = DIM,
	.height       = DIM,
	.stride_bytes = DIM * 4U,
	.format       = ALP_GPU2D_FMT_ARGB8888,
};

/* Pixel at (x,y) in an ARGB8888 uint32 surface. */
static inline uint32_t px(const uint32_t *buf, uint32_t x, uint32_t y)
{
	return buf[y * DIM + x];
}

static uint32_t fails;

/* Assert one pixel equals an expected ARGB8888 value; log + count mismatches. */
static void chk_px(const char *what, const uint32_t *buf, uint32_t x, uint32_t y, uint32_t expect)
{
	uint32_t got = px(buf, x, y);

	if (got != expect) {
		fails += 1U;
		printk("  FAIL %s @(%u,%u): got=0x%08x expect=0x%08x\n", what, x, y, got, expect);
	}
}

/* Blended channels may differ by 1 between backends (<alp/gpu2d.h>). */
static void
chk_px_blend(const char *what, const uint32_t *buf, uint32_t x, uint32_t y, uint32_t expect)
{
	uint32_t got = px(buf, x, y);

	for (int sh = 0; sh < 32; sh += 8) {
		int d = (int)((got >> sh) & 0xFFU) - (int)((expect >> sh) & 0xFFU);

		if (d > 1 || d < -1) {
			printk("  FAIL %s @(%u,%u): got=0x%08x expect=0x%08x (+-1/channel)\n",
			       what,
			       x,
			       y,
			       got,
			       expect);
			fails++;
			return;
		}
	}
}

/* Assert a status is ALP_OK; log + count mismatches. */
static void chk_ok(const char *what, alp_status_t rc)
{
	if (rc != ALP_OK) {
		fails += 1U;
		printk("  FAIL %s: rc=%d\n", what, (int)rc);
	}
}

int main(void)
{
	printk("\n=== AEN GPU2D bench (<alp/gpu2d.h>, " BACKEND ") ===\n");

	alp_gpu2d_t *g = alp_gpu2d_open();

	if (g == NULL) {
		printk("RESULT FAIL: alp_gpu2d_open() returned NULL (last_error=%d)\n",
		       (int)alp_last_error());
		return 0;
	}
	printk("alp_gpu2d_open() OK\n");

	/* 1. fill_rect: solid fill of the whole dst, then a clipped fill. */
	chk_ok("fill full", alp_gpu2d_fill_rect(g, &dst, 0, 0, DIM, DIM, 0xFF112233U));
	chk_px("fill full", dst_buf, 0, 0, 0xFF112233U);
	chk_px("fill full", dst_buf, DIM - 1U, DIM - 1U, 0xFF112233U);

	/* Clipped fill: a 10x10 rect at (6,6) on an 8x8 surface touches only the
	 * 2x2 in-bounds corner; pixels outside it keep the previous fill. */
	chk_ok("fill clip", alp_gpu2d_fill_rect(g, &dst, 6, 6, 10, 10, 0xFFAABBCCU));
	chk_px("fill clip in", dst_buf, 7, 7, 0xFFAABBCCU);
	chk_px("fill clip out", dst_buf, 5, 5, 0xFF112233U);

	/* 2. blit: copy a 4x4 sub-rect from src(0,0) into dst(2,2). */
	chk_ok("src prep", alp_gpu2d_fill_rect(g, &src, 0, 0, DIM, DIM, 0xFF445566U));
	chk_ok("dst prep", alp_gpu2d_fill_rect(g, &dst, 0, 0, DIM, DIM, 0xFF000000U));
	chk_ok("blit", alp_gpu2d_blit(g, &src, 0, 0, &dst, 2, 2, 4, 4));
	chk_px("blit in tl", dst_buf, 2, 2, 0xFF445566U);
	chk_px("blit in br", dst_buf, 5, 5, 0xFF445566U);
	chk_px("blit out", dst_buf, 0, 0, 0xFF000000U); /* outside the copied rect */
	chk_px("blit out", dst_buf, 6, 6, 0xFF000000U);

	/* 3. blend REPLACE: src pixel overwrites dst exactly. */
	chk_ok("blend prep dst", alp_gpu2d_fill_rect(g, &dst, 0, 0, 1, 1, 0xFFFFFFFFU));
	chk_ok("blend prep src", alp_gpu2d_fill_rect(g, &src, 0, 0, 1, 1, 0x12345678U));
	chk_ok("blend REPLACE",
	       alp_gpu2d_blend(g, &src, 0, 0, &dst, 0, 0, 1, 1, ALP_GPU2D_BLEND_REPLACE));
	chk_px("blend REPLACE", dst_buf, 0, 0, 0x12345678U);

	/* 4. blend SRC_OVER: 50%-alpha red over opaque blue.
	 *    sa=0x80 -> out_r=(255*128+127)/255=128, out_b=(255*127+127)/255=127,
	 *    out_a=128+(255*127+127)/255=255  ->  0xFF80007F. */
	chk_ok("blend prep dst", alp_gpu2d_fill_rect(g, &dst, 0, 0, 1, 1, 0xFF0000FFU));
	chk_ok("blend prep src", alp_gpu2d_fill_rect(g, &src, 0, 0, 1, 1, 0x80FF0000U));
	chk_ok("blend SRC_OVER",
	       alp_gpu2d_blend(g, &src, 0, 0, &dst, 0, 0, 1, 1, ALP_GPU2D_BLEND_SRC_OVER));
	chk_px_blend("blend SRC_OVER", dst_buf, 0, 0, 0xFF80007FU);

	/* 5. blend ADDITIVE: per-channel clamped add. 0x80402010 + 0x10203040
	 *    = 0x90605050 (no channel saturates). */
	chk_ok("blend prep dst", alp_gpu2d_fill_rect(g, &dst, 0, 0, 1, 1, 0x10203040U));
	chk_ok("blend prep src", alp_gpu2d_fill_rect(g, &src, 0, 0, 1, 1, 0x80402010U));
	chk_ok("blend ADDITIVE",
	       alp_gpu2d_blend(g, &src, 0, 0, &dst, 0, 0, 1, 1, ALP_GPU2D_BLEND_ADDITIVE));
	chk_px("blend ADDITIVE", dst_buf, 0, 0, 0x90605050U);

	/* ADDITIVE saturation: 0xFFFFFFFF + 0x01010101 clamps to 0xFFFFFFFF. */
	chk_ok("blend prep dst", alp_gpu2d_fill_rect(g, &dst, 0, 0, 1, 1, 0x01010101U));
	chk_ok("blend prep src", alp_gpu2d_fill_rect(g, &src, 0, 0, 1, 1, 0xFFFFFFFFU));
	chk_ok("blend ADD sat",
	       alp_gpu2d_blend(g, &src, 0, 0, &dst, 0, 0, 1, 1, ALP_GPU2D_BLEND_ADDITIVE));
	chk_px("blend ADD sat", dst_buf, 0, 0, 0xFFFFFFFFU);

	/* 6. blend MULTIPLY: white src * 0x808080 dst = 0x80808080
	 *    (rgb (255*128+127)/255=128; a (255*255+127)/255=255). */
	chk_ok("blend prep dst", alp_gpu2d_fill_rect(g, &dst, 0, 0, 1, 1, 0xFF808080U));
	chk_ok("blend prep src", alp_gpu2d_fill_rect(g, &src, 0, 0, 1, 1, 0xFFFFFFFFU));
	chk_ok("blend MULTIPLY",
	       alp_gpu2d_blend(g, &src, 0, 0, &dst, 0, 0, 1, 1, ALP_GPU2D_BLEND_MULTIPLY));
	chk_px("blend MULTIPLY", dst_buf, 0, 0, 0xFF808080U);

	alp_gpu2d_close(g);

	if (fails == 0U) {
		printk("RESULT PASS: GPU2D " BACKEND " fill/blit/blend "
		       "(REPLACE/SRC_OVER/ADDITIVE/MULTIPLY) all match on E8\n");
	} else {
		printk("RESULT FAIL: %u GPU2D check(s) mismatched\n", fails);
	}
	return 0;
}
