/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gfx-compat-blit -- using gfx_compat, the in-tree RGB565 fill/blit
 * shim (src/lib/gfx_compat/), directly.
 *
 * Unlike the other library examples in examples/display/, gfx_compat
 * isn't a fetched third-party dependency -- `west.yml` documents it as
 * shipping in-tree, and `board.yaml`'s `libraries: [gfx_compat]` just
 * enables the CONFIG_ALP_GFX_COMPAT_SW Kconfig knob (see
 * zephyr/Kconfig.alp-libraries) that compiles src/lib/gfx_compat/
 * gfx_compat.c into the build. `#include "gfx_compat.h"` below then
 * resolves the same way it would for any Zephyr-module-provided header
 * -- no <alp/...> wrapper indirection.
 *
 * gfx_compat's API is deliberately tiny: fill a RGB565 buffer, or copy
 * one buffer into another of the SAME w*h size. Neither call knows
 * about a bigger framebuffer's stride -- see the blit loop below for
 * the pattern gfx_compat.h's doc comment describes for placing a small
 * "patch" into a larger canvas.
 */

#include <stdint.h>
#include <stdio.h>

#include "gfx_compat.h"

/* A small RGB565 "framebuffer" -- 16x16 pixels, entirely on the stack.
 * gfx_compat has no allocator of its own (no heap on the hot path is
 * an SDK-wide invariant -- see docs/recommended-libraries.md), so
 * every buffer in this example is caller-owned. */
#define CANVAS_WIDTH  16
#define CANVAS_HEIGHT 16

/* The "patch" blitted into the canvas -- smaller than the canvas so
 * the blit loop below has to do real placement, not just a full-buffer
 * copy. */
#define PATCH_WIDTH  4
#define PATCH_HEIGHT 4
#define PATCH_X      6 /* top-left corner inside the canvas */
#define PATCH_Y      6

/* RGB565: 5 bits red, 6 bits green, 5 bits blue, packed MSB-first.
 * These two are pure red and pure green at full intensity -- easy to
 * tell apart in the checksum without needing a real display. */
#define COLOR_RED   0xF800u
#define COLOR_GREEN 0x07E0u

int main(void)
{
	static uint16_t canvas[CANVAS_WIDTH * CANVAS_HEIGHT];
	static uint16_t patch[PATCH_WIDTH * PATCH_HEIGHT];

	/* Step 1: fill. gfx_compat_fill treats its buffer as flat and
	 * contiguous -- no stride parameter -- so it's only usable
	 * directly on a buffer that IS the whole surface, like both of
	 * these are. */
	gfx_compat_fill(canvas, CANVAS_WIDTH, CANVAS_HEIGHT, COLOR_RED);
	gfx_compat_fill(patch, PATCH_WIDTH, PATCH_HEIGHT, COLOR_GREEN);

	/* Step 2: blit the patch into the canvas at (PATCH_X, PATCH_Y).
	 * gfx_compat_blit's contract (see gfx_compat.h) is the same
	 * "no stride" shape as fill: it copies exactly w*h contiguous
	 * pixels from src to dst. That's fine for a full-buffer copy, but
	 * placing a smaller rect INSIDE a larger canvas means the
	 * canvas's rows aren't contiguous with each other from the
	 * patch's point of view -- row N of the canvas starts
	 * CANVAS_WIDTH pixels after row N-1, not PATCH_WIDTH pixels
	 * after. The fix is what every stride-unaware blit primitive
	 * expects the caller to do: call it once per row, with pointer
	 * arithmetic picking out each row's starting pixel and a height
	 * of 1. */
	for (int row = 0; row < PATCH_HEIGHT; row++) {
		uint16_t       *dst_row = &canvas[(PATCH_Y + row) * CANVAS_WIDTH + PATCH_X];
		const uint16_t *src_row = &patch[row * PATCH_WIDTH];

		gfx_compat_blit(dst_row, src_row, PATCH_WIDTH, 1);
	}

	/* Step 3: checksum instead of a real display. A plain additive
	 * checksum over all CANVAS_WIDTH*CANVAS_HEIGHT pixels is enough
	 * to prove the fill + blit actually landed the expected colors in
	 * the expected place -- change PATCH_X/Y/WIDTH/HEIGHT and this
	 * number moves, which is the point (it is NOT a cryptographic or
	 * error-detecting checksum, just a cheap "did anything change"
	 * signal for a console-only example). */
	uint32_t checksum = 0;

	for (int i = 0; i < CANVAS_WIDTH * CANVAS_HEIGHT; i++) {
		checksum += canvas[i];
	}

	printf("[gfx-compat-blit] canvas %dx%d, patch %dx%d at (%d,%d)\n",
	       CANVAS_WIDTH,
	       CANVAS_HEIGHT,
	       PATCH_WIDTH,
	       PATCH_HEIGHT,
	       PATCH_X,
	       PATCH_Y);
	printf("[gfx-compat-blit] checksum=0x%08x\n", checksum);

	printf("[gfx-compat-blit] done\n");
	return 0;
}
