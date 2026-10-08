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
 *
 * Bench run 312 (E1M-AEN803 + IMX296, 1280x960, issue #2287) found the
 * retry never actually engaged: main.c's old gate keyed off the ENCODER's
 * reported required size (out_len), which one of ALP_ERR_NOMEM's two
 * backend-internal causes (src/backends/jpeg/alif_hantro.c's JPEG_BUFFER_
 * FULL / -ENOSPC path) never set, leaving it 0 and the gate always false.
 * jpeg_quality_should_retry() below now gates on the error alone (still
 * bounded to one retry, still floored) -- simpler, and correct for both
 * of ALP_ERR_NOMEM's causes, not just the one that happened to set
 * out_len. main.c also now PERSISTS the ladder's current rung across
 * frames instead of resetting to the default every frame (see its own
 * comment), and jpeg_quality_step_up() below is the pure step used to
 * recover back toward the default after a streak of clean encodes.
 */

#ifndef JPEG_QUALITY_LADDER_H
#define JPEG_QUALITY_LADDER_H

#include <stdbool.h>
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

/**
 * @brief One step UP the quality ladder from @p quality, toward @p ceiling.
 *
 * The recovery half of the ladder (bench run 312, #2287): after a streak of
 * clean encodes at a rung below @p ceiling (main.c's caller decides how long
 * a streak counts), climb back one rung instead of staying parked at
 * whatever rung a past failure left it at forever. Ceilinged at @p ceiling
 * -- never returns above it, and never wraps on a @p quality already at or
 * above @p ceiling (mirrors jpeg_quality_step_down()'s own floor guarantee).
 *
 * @param quality  Current quality, 1..100.
 * @param ceiling  Upper bound to climb back toward (main.c's
 *                 JPEG_QUALITY_DEFAULT).
 * @return The next, higher rung, never above @p ceiling.
 */
static inline uint8_t jpeg_quality_step_up(uint8_t quality, uint8_t ceiling)
{
	if (quality >= ceiling) {
		return ceiling;
	}
	int stepped = (int)quality + (int)JPEG_QUALITY_STEP;

	return (uint8_t)(stepped < (int)ceiling ? stepped : (int)ceiling);
}

/**
 * @brief Should a failed encode be retried at a lower quality?
 *
 * Bench run 312 (#2287): gates on the error alone, not on the encoder's
 * reported required size (out_len) -- see this header's own file comment
 * for why out_len is not a reliable signal for one of ALP_ERR_NOMEM's two
 * backend-internal causes. Takes a plain bool rather than alp_status_t so
 * this header stays dependency-free (no alp/ include) the same way
 * jpeg_quality_step_down() already is.
 *
 * @param is_nomem_error  True if the encode failed with ALP_ERR_NOMEM.
 * @param quality         The quality the failed attempt just used.
 * @return True if a retry at jpeg_quality_step_down(quality) is worth
 *         attempting -- false once @p quality is already at the floor (a
 *         retry at the exact quality that just failed can't help).
 */
static inline bool jpeg_quality_should_retry(bool is_nomem_error, uint8_t quality)
{
	return is_nomem_error && quality > JPEG_QUALITY_FLOOR;
}

#endif /* JPEG_QUALITY_LADDER_H */
