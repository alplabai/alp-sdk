/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for jpeg_quality_step_down() (examples/connectivity/
 * camera-mjpeg-stream/src/jpeg_quality_ladder.h) -- the retry-ladder step
 * used when alp_jpeg_encode() returns ALP_ERR_NOMEM (Hantro output-buffer
 * overrun, issue #2286 bench run 242).  Pins the floor and the no-wrap
 * behaviour a naive `quality - JPEG_QUALITY_STEP` would get wrong.
 */

#include <zephyr/ztest.h>

#include "jpeg_quality_ladder.h"

ZTEST_SUITE(mjpeg_quality_ladder, NULL, NULL, NULL, NULL, NULL);

ZTEST(mjpeg_quality_ladder, test_steps_down_by_one_rung)
{
	zassert_equal(jpeg_quality_step_down(80), 60);
	zassert_equal(jpeg_quality_step_down(60), 40);
}

ZTEST(mjpeg_quality_ladder, test_floors_instead_of_wrapping)
{
	/* A plain `quality - JPEG_QUALITY_STEP` on a uint8_t already at or
	 * below the floor would underflow to a huge value instead of
	 * clamping -- this is the bug the floor check exists to prevent. */
	zassert_equal(jpeg_quality_step_down(JPEG_QUALITY_FLOOR), JPEG_QUALITY_FLOOR);
	zassert_equal(jpeg_quality_step_down(10), JPEG_QUALITY_FLOOR);
	zassert_equal(jpeg_quality_step_down(1), JPEG_QUALITY_FLOOR);
}

ZTEST(mjpeg_quality_ladder, test_is_idempotent_at_the_floor)
{
	uint8_t q = 45;

	q = jpeg_quality_step_down(q);
	zassert_equal(q, JPEG_QUALITY_FLOOR);
	q = jpeg_quality_step_down(q); /* a bounded retry loop calling this twice must not wrap */
	zassert_equal(q, JPEG_QUALITY_FLOOR);
}
