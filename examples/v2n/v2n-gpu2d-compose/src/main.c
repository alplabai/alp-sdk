/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n-gpu2d-compose -- compose ARGB8888 layers through the portable
 * <alp/gpu2d.h> surface and check the result.
 *
 * What this example shows
 * =======================
 *
 *   1. alp_gpu2d_open() never asks "which GPU is this?".  The SDK picks the
 *      backend: on an RZ/V2N image that carries the Renesas Mali-G31 stack
 *      the ops run on the GPU through EGL/GLES; anywhere else (or when no
 *      compositor/GPU context is available) the identical calls run on the
 *      CPU.  Same binary, same source, same pixels (within 1 per channel
 *      for blends -- see <alp/gpu2d.h>).
 *   2. How an app tells which one it got: the instance capabilities.  A
 *      hardware backend sets ALP_INSTANCE_CAP_DMA; the CPU path does not.
 *      That flag only says a GPU context came up -- an individual op can
 *      still fall back to the CPU (the GLES backend then prints
 *      "[gpu2d/gles] ..." lines on stderr, including op totals at close).
 *      Use this for diagnostics and timing only -- never to branch the
 *      drawing code.
 *   3. The three operations: fill_rect (solid colour), blit (copy) and
 *      blend (SRC_OVER here), all on caller-owned memory described by an
 *      alp_gpu2d_surface_t.
 *
 * Surfaces are plain malloc'd ARGB8888 buffers.  ARGB8888 packs
 * little-endian as B,G,R,A, so reading a uint32 pixel gives 0xAARRGGBB.
 * The surface is deliberately large (640x360): the GPU path uploads the
 * source, draws, and reads the result back, so tiny rects are served by the
 * CPU even on a GPU build (the SDK delegates them op by op).
 *
 * Output (stdout), one line per step, ending in "[gpu2d] PASS" or
 * "[gpu2d] FAIL ..." with a matching exit status -- the HIL spec
 * tests/hil/v2m103-x-evk/v2n-gpu2d-compose.yaml greps for these.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include <alp/gpu2d.h>
#include <alp/peripheral.h>

#define W 640u
#define H 360u

/* Wrap a malloc'd buffer in the descriptor every gpu2d call takes.  The
 * descriptor is copied per call, so it can live on the stack. */
static alp_gpu2d_surface_t make_surface(uint32_t *px)
{
	alp_gpu2d_surface_t s = {
		.base         = px,
		.width        = W,
		.height       = H,
		.stride_bytes = W * 4u, /* tightly packed: width * bytes-per-pixel */
		.format       = ALP_GPU2D_FMT_ARGB8888,
	};
	return s;
}

/* Straight-alpha SRC_OVER of one channel, rounded -- the formula
 * <alp/gpu2d.h> documents.  Used to compute the expected value. */
static uint32_t over(uint32_t s, uint32_t d, uint32_t sa)
{
	return (s * sa + d * (255u - sa) + 127u) / 255u;
}

static int near_to(uint32_t got, uint32_t want)
{
	/* Backends may differ by 1 per channel on blends (GPU /256 vs CPU /255). */
	return got + 1u >= want && got <= want + 1u;
}

static double ms_since(const struct timespec *t0)
{
	struct timespec t1;
	clock_gettime(CLOCK_MONOTONIC, &t1);
	return (double)(t1.tv_sec - t0->tv_sec) * 1e3 + (double)(t1.tv_nsec - t0->tv_nsec) / 1e6;
}

int main(void)
{
	int       result = 1;
	uint32_t *bg     = malloc((size_t)W * H * 4u);
	uint32_t *layer  = malloc((size_t)W * H * 4u);
	uint32_t *out    = malloc((size_t)W * H * 4u);
	if (bg == NULL || layer == NULL || out == NULL) {
		printf("[gpu2d] FAIL out of memory\n");
		goto free_bufs;
	}

	alp_gpu2d_t *g = alp_gpu2d_open();
	if (g == NULL) {
		printf("[gpu2d] FAIL open returned NULL (alp_last_error=%d)\n", (int)alp_last_error());
		goto free_bufs;
	}
	const alp_capabilities_t *caps = alp_gpu2d_capabilities(g);
	printf("[gpu2d] engine: %s\n",
	       (caps != NULL && (caps->flags & ALP_INSTANCE_CAP_DMA)) ? "GPU context up (EGL/GLES)"
	                                                              : "CPU fallback");

	const alp_gpu2d_surface_t sbg = make_surface(bg), slayer = make_surface(layer),
	                          sout = make_surface(out);

	/* Opaque dark-blue background and a 50 % red layer (alpha 0x80). */
	const uint32_t bg_argb = 0xFF102030u, layer_argb = 0x80FF0000u;
	alp_status_t   rc = alp_gpu2d_fill_rect(g, &sbg, 0, 0, W, H, bg_argb);
	rc |= alp_gpu2d_fill_rect(g, &slayer, 0, 0, W, H, layer_argb);
	/* Copy the background to the output, then composite the layer over it. */
	rc |= alp_gpu2d_blit(g, &sbg, 0, 0, &sout, 0, 0, W, H);
	rc |= alp_gpu2d_blend(g, &slayer, 0, 0, &sout, 0, 0, W, H, ALP_GPU2D_BLEND_SRC_OVER);
	if (rc != ALP_OK) {
		printf("[gpu2d] FAIL op status=%d\n", (int)rc);
		goto close_gpu;
	}

	/* Check one pixel in the middle against the documented formula. */
	const uint32_t px = out[(H / 2u) * W + W / 2u];
	const uint32_t sa = layer_argb >> 24;
	const uint32_t er = over((layer_argb >> 16) & 0xFFu, (bg_argb >> 16) & 0xFFu, sa);
	const uint32_t eg = over((layer_argb >> 8) & 0xFFu, (bg_argb >> 8) & 0xFFu, sa);
	const uint32_t eb = over(layer_argb & 0xFFu, bg_argb & 0xFFu, sa);
	const int      ok = near_to((px >> 16) & 0xFFu, er) && near_to((px >> 8) & 0xFFu, eg) &&
	                    near_to(px & 0xFFu, eb) && (px >> 24) == 0xFFu;
	printf("[gpu2d] centre pixel 0x%08X, expected ~0x%02X%02X%02X%02X\n",
	       (unsigned)px,
	       0xFFu,
	       (unsigned)er,
	       (unsigned)eg,
	       (unsigned)eb);

	/* Timing: 20 full-surface blends.  A number to compare engines with,
	 * not a benchmark -- the first call includes any one-time GPU setup. */
	struct timespec t0;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (int i = 0; i < 20; ++i) {
		rc = alp_gpu2d_blend(g, &slayer, 0, 0, &sout, 0, 0, W, H, ALP_GPU2D_BLEND_SRC_OVER);
		if (rc != ALP_OK) {
			printf("[gpu2d] FAIL timed blend %d status=%d\n", i, (int)rc);
			goto close_gpu;
		}
	}
	printf("[gpu2d] 20 x blend %ux%u: %.2f ms total\n", W, H, ms_since(&t0));

	printf(ok ? "[gpu2d] PASS\n" : "[gpu2d] FAIL pixel mismatch\n");
	result = ok ? 0 : 1;

close_gpu:
	alp_gpu2d_close(g);
free_bufs:
	free(bg);
	free(layer);
	free(out);
	return result;
}
