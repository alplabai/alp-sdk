/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure, dependency-free JPEG-quality step-down ladder for the
 * camera-capture loop in main.c.
 *
 * Bench run 242 (E1M-AEN803 + OV5647, 1280x960 @ 15 fps, issue #2286): once
 * AE converged past the first ~12 dark frames, EVERY encode failed with
 * ALP_ERR_NOMEM (the Hantro driver's -ENOSPC JPEG_BUFFER_FULL --
 * src/backends/jpeg/alif_hantro.c -- mapped through
 * src/common/alp_errno.h's alp_status_from_zephyr_errno()) -- a brighter
 * NV12 scene at a fixed quality can exceed MJPEG_HTTP_MAX_JPEG
 * (mjpeg_http.h), and at 1280x960 that cap has no SRAM0 headroom left to
 * grow. main.c now retries a failed encode ONCE at a lower quality,
 * stepping down this ladder, rather than dropping the frame outright.
 */

#ifndef JPEG_QUALITY_LADDER_H
#define JPEG_QUALITY_LADDER_H

#include <stdint.h>

/** Lowest quality the retry ladder steps down to -- below this, JPEG
 *  blocking artefacts start to dominate the image; not worth chasing a
 *  smaller file past this point. */
#define JPEG_QUALITY_FLOOR 40u

/** One rung's drop in quality. */
#define JPEG_QUALITY_STEP 20u

/**
 * @brief One step down the quality ladder from @p quality.
 *
 * Floored at @ref JPEG_QUALITY_FLOOR -- never returns below it, and never
 * wraps (unlike a plain `quality - JPEG_QUALITY_STEP` on a `uint8_t`
 * already at or below the floor, which would underflow).  Idempotent at
 * the floor: stepping down from @ref JPEG_QUALITY_FLOOR returns
 * @ref JPEG_QUALITY_FLOOR again, so a caller can call this in a bounded
 * retry loop without a separate floor check.
 *
 * @param quality  Current quality, 1..100 (see alp_jpeg_encode_req_t::quality).
 * @return The next, lower rung.
 */
static inline uint8_t jpeg_quality_step_down(uint8_t quality)
{
	if (quality <= JPEG_QUALITY_FLOOR) {
		return JPEG_QUALITY_FLOOR;
	}
	int stepped = (int)quality - (int)JPEG_QUALITY_STEP;

	return (uint8_t)(stepped > (int)JPEG_QUALITY_FLOOR ? stepped : (int)JPEG_QUALITY_FLOOR);
}

#endif /* JPEG_QUALITY_LADDER_H */
