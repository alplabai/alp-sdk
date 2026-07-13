/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gfx_compat pure-C SW-blit fallback (CONFIG_ALP_GFX_COMPAT_SW).
 *
 * See gfx_compat.h for the API contract. Deliberately dumb: O(w*h)
 * scalar loops, no SIMD, no DMA, no framebuffer stride handling --
 * this is the always-available floor every silicon target can fall
 * back to, mirroring the other libraries' *_PURE_C / *_SW_BLIT
 * fallback shape (see zephyr/Kconfig.alp-libraries).
 */

#include "gfx_compat.h"

#include <stddef.h>

void gfx_compat_fill(uint16_t *buf, int w, int h, uint16_t color)
{
	if (buf == NULL || w <= 0 || h <= 0) {
		return;
	}

	long count = (long)w * (long)h;

	for (long i = 0; i < count; i++) {
		buf[i] = color;
	}
}

void gfx_compat_blit(uint16_t *dst, const uint16_t *src, int w, int h)
{
	if (dst == NULL || src == NULL || w <= 0 || h <= 0) {
		return;
	}

	long count = (long)w * (long)h;

	for (long i = 0; i < count; i++) {
		dst[i] = src[i];
	}
}
