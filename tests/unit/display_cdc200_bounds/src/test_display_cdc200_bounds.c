/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #1121: display_cdc200.c's read/write entry points (generic and indexed)
 * computed the framebuffer offset from (x, y, desc) with no validation -- a
 * rectangle extending past the layer's window, an undersized caller buffer,
 * or an invalid pitch would over/under-run the framebuffer or the caller's
 * buffer. Boundary-tested here against the exact cdc200_validate_transfer()
 * guard every entry point now calls, on the host, with no MMIO/devicetree
 * involved.
 */
#include <zephyr/drivers/display.h>
#include <zephyr/ztest.h>

#include "display_cdc200.h"

ZTEST_SUITE(display_cdc200_bounds, NULL, NULL, NULL, NULL, NULL);

/* A 100x80 ARGB8888 (4 bytes/px) layer window at the origin. */
static const struct cdc200_layer_config layer100x80 = {
	.pixel_size = 4,
	.x0         = 0,
	.x1         = 100,
	.y0         = 0,
	.y1         = 80,
};

struct transfer_case {
	const char *name;
	uint16_t    x;
	uint16_t    y;
	uint16_t    width;
	uint16_t    height;
	uint16_t    pitch;
	uint32_t    buf_size;
	bool        want_ok;
};

static const struct transfer_case cases[] = {
	/* Exact full-window transfer -- valid. */
	{ "full window, exact buffer", 0, 0, 100, 80, 100, 100u * 80u * 4u, true },

	/* Zero-size transfer at the far corner (x==width, y==height) is a
	 * degenerate no-op and must be accepted.
	 */
	{ "zero-size transfer at the far corner", 100, 80, 0, 0, 0, 0, true },

	/* Edge: rectangle ends exactly at the window boundary. */
	{ "rectangle ends exactly at edge", 50, 40, 50, 40, 50, 50u * 40u * 4u, true },

	/* One-past-edge in x. */
	{ "one column past right edge", 1, 0, 100, 80, 100, 100u * 80u * 4u, false },

	/* One-past-edge in y. */
	{ "one row past bottom edge", 0, 1, 100, 80, 100, 100u * 80u * 4u, false },

	/* x already past the window (short-circuit before the subtraction). */
	{ "x already past window width", 101, 0, 1, 1, 1, 4u, false },
	{ "y already past window height", 0, 81, 1, 1, 1, 4u, false },

	/* Undersized caller buffer for an otherwise-valid rectangle. */
	{ "undersized buffer", 0, 0, 100, 80, 100, (100u * 80u * 4u) - 1u, false },

	/* Invalid pitch: narrower than the claimed row width. */
	{ "pitch smaller than width", 0, 0, 100, 80, 99, 100u * 80u * 4u, false },

	/* Valid: pitch wider than width (caller's buffer has row padding),
	 * with buf_size sized to the pitch (not just the width).
	 */
	{ "pitch wider than width, buffer sized to pitch", 0, 0, 100, 80, 120, 120u * 80u * 4u, true },
};

ZTEST(display_cdc200_bounds, test_validate_transfer_table)
{
	for (size_t i = 0; i < ARRAY_SIZE(cases); i++) {
		const struct transfer_case      *tc   = &cases[i];
		struct display_buffer_descriptor desc = {
			.buf_size = tc->buf_size,
			.width    = tc->width,
			.height   = tc->height,
			.pitch    = tc->pitch,
		};
		int ret = cdc200_validate_transfer(&layer100x80, tc->x, tc->y, &desc);

		if (tc->want_ok) {
			zassert_equal(ret, 0, "case '%s': expected valid, got %d", tc->name, ret);
		} else {
			zassert_equal(ret, -EINVAL, "case '%s': expected -EINVAL, got %d", tc->name, ret);
		}
	}
}

/* Arithmetic-overflow break-test: a pitch*pixel_size*height product that
 * would overflow a 32-bit accumulator must still be correctly rejected
 * against a buffer far too small to hold it -- proving the check is done in
 * a wide-enough intermediate, not one that silently wraps and accepts.
 */
ZTEST(display_cdc200_bounds, test_overflow_prone_product_is_rejected)
{
	static const struct cdc200_layer_config huge_layer = {
		.pixel_size = 4,
		.x0         = 0,
		.x1         = 65535,
		.y0         = 0,
		.y1         = 65535,
	};
	struct display_buffer_descriptor desc = {
		.buf_size = UINT32_MAX, /* the largest representable buf_size */
		.width    = 65535,
		.height   = 65535,
		.pitch    = 65535,
	};

	/* Real requirement is 65535*65535*4 ~= 1.7e10, which does not fit in
	 * a uint32_t (it would wrap to a small value there) but is nowhere
	 * near UINT32_MAX either way -- must reject.
	 */
	zassert_equal(cdc200_validate_transfer(&huge_layer, 0, 0, &desc),
	              -EINVAL,
	              "an overflow-prone pitch*pixel_size*height must still be rejected");
}
