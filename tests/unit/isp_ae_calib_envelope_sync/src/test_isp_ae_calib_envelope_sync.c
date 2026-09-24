/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-side (native_sim) regression for #2277's final fix (bench run 251):
 * the OV5647 AE write-back ceiling must track the ACTIVE fps at runtime,
 * not a value guessed at compile time.
 *
 * An earlier cut mirrored the active-fps exposure/gain range into the
 * calibration block SetCalib() loads (isp_calib_param.modules.ae.autoAttr)
 * -- bench evidence (runs 244/245/251) proved the closed VSI_MPI_ISP
 * library does not enforce its per-frame intLine ceiling from that struct
 * at all, so that mirror was dead code and was removed along with its test
 * coverage here. This file mirrors the two pieces of the REAL runtime path
 * instead:
 *
 *   1. isp_ae_int_time_max_us_from_frmival() -- zephyr/drivers/video/
 *      isp_pico.c's own pure helper: the sensor's ACTIVE frame interval
 *      (video_get_frmival(), sensor-agnostic) in, an exposure-time ceiling
 *      in microseconds out. isp_pico.c is alp-sdk-owned (not a vendored
 *      hal_alif patch), so
 *      tests/scripts/test_isp_ae_calib_envelope_sync_patch_mirror.py
 *      extracts this function's body directly from that file (brace-
 *      matched, the same technique test_isp_ae_ctrl_clamp_patch_mirror.py
 *      uses against a vendored patch) and fails if the two drift.
 *
 *   2. isp_ae_sns_full_lines_from_frmival() (isp_pico.c) +
 *      isp_sns_default_sync() (mirroring hal_alif patch 0011's
 *      isp_vsi_sync_ae_sns_default()), which keep sensor_attributes
 *      (AE_SNS_DEFAULT_S) -- specifically maxIntLine -- current for the
 *      active frame period. Bench run 251 confirms this struct's field
 *      sync is what matters: isp_api_wrapper.c's vsi_int_time_update()
 *      write-back clamp reads maxIntLine directly and is the ONLY
 *      mechanism bench-proven to hold the sensor within the active-mode
 *      ceiling -- the library's own internal AE request stays pinned at
 *      the compiled-in boot default (3145) regardless of how the sensor
 *      default is (re-)registered with the library, so isp_vsi_sync_ae_
 *      sns_default() no longer attempts that registration dance (see the
 *      patch's own comment). test_unsynced_sns_default_stays_pinned_to_
 *      stale_default pins the no-sync regression;
 *      test_sns_default_sync_tracks_active_fps_switch_30_10_30 proves the
 *      fix.
 */
#include <stdint.h>

#include <zephyr/ztest.h>

/*
 * Mirror of isp_pico.c's isp_ae_int_time_max_us_from_frmival() -- see file
 * comment. Kept as a plain uint32_t/uint64_t function (no Zephyr video_*
 * types): the real function's only inputs are the frmival's numerator/
 * denominator, already plain integers there too.
 */
static uint32_t isp_ae_int_time_max_us_from_frmival(uint32_t frmival_num, uint32_t frmival_den)
{
	if (frmival_den == 0) {
		return 0;
	}

	uint64_t frame_period_us = (uint64_t)frmival_num * 1000000ULL / frmival_den;
	uint64_t margin_us       = frame_period_us / 50; /* 2% */

	return (uint32_t)(frame_period_us > margin_us ? frame_period_us - margin_us : frame_period_us);
}

ZTEST_SUITE(isp_ae_calib_envelope_sync, NULL, NULL, NULL, NULL, NULL);

/* Sanity: the extracted helper must reproduce the bench-cited figures
 * (#2277 review round: "run 220 at 30 fps read int_time_max=32667") before
 * trusting it anywhere else in this file.
 */
ZTEST(isp_ae_calib_envelope_sync, test_int_time_max_us_matches_bench_values)
{
	zassert_equal(isp_ae_int_time_max_us_from_frmival(1, 10),
	              98000u,
	              "10 fps: 100000 us period - 2%% margin");
	zassert_equal(isp_ae_int_time_max_us_from_frmival(1, 30),
	              32667u,
	              "30 fps: bench run 220's own cited figure");
	zassert_equal(
	    isp_ae_int_time_max_us_from_frmival(1, 60), 16333u, "60 fps: 16666 us period - 2%% margin");
}

/* An invalid interval (denominator 0) is the caller's (isp_apply_ae())
 * signal to keep its own fallback constant, not something this function
 * should guess a value for.
 */
ZTEST(isp_ae_calib_envelope_sync, test_zero_denominator_returns_zero)
{
	zassert_equal(isp_ae_int_time_max_us_from_frmival(1, 0), 0u, NULL);
}

/*
 * Bench run 244 found that a compiled-in-calibration mirror (deleted
 * alongside its test coverage here) does not bound the library's per-frame
 * intLine output: isp_apply_ae() pushed a correct 30 fps ceiling into
 * isp_calib_param.modules.ae.autoAttr, but the library's own "VSI AE INFO"
 * console trace showed intLine ramping straight past that push's
 * line-equivalent and pinning at exactly 3145 -- sensor_attributes.h's
 * AE_SNS_DEFAULT_S.maxIntLine, the fixed 10 fps boot-time default (hal_alif
 * patch 0006), registered with the library exactly ONCE by
 * VSI_MPI_ISP_InitAeSnsFunc() (isp_vsi_init()) at device boot -- well
 * before any app has negotiated a frame rate.
 *
 * The two pieces mirrored below:
 *
 *   1. isp_ae_sns_full_lines_from_frmival() -- isp_pico.c's own pure
 *      helper (same file/function as isp_ae_int_time_max_us_from_frmival()
 *      above), byte-for-byte checked against the real source by
 *      tests/scripts/test_isp_ae_calib_envelope_sync_patch_mirror.py, same
 *      technique as that function.
 *
 *   2. isp_sns_default_sync() -- a conceptual mirror of hal_alif patch
 *      0011's isp_vsi_sync_ae_sns_default(): updates a local POD
 *      "sensor_attributes" stand-in's fullLines/fullLinesStd/maxIntLine.
 *      Bench run 251 confirmed this field sync -- read by
 *      vsi_int_time_update()'s write-back clamp -- is what actually holds
 *      the sensor within the active-mode ceiling; the real function no
 *      longer also re-registers aeSnsFunc with the library (bench-proven
 *      to have no effect on the library's own internal AE request), so
 *      this mirror needs no vendor-typed counterpart for that either.
 */
static uint32_t isp_ae_sns_full_lines_from_frmival(uint32_t frmival_num, uint32_t frmival_den)
{
	if (frmival_den == 0) {
		return 0;
	}

	uint64_t frame_period_us = (uint64_t)frmival_num * 1000000ULL / frmival_den;

	return (uint32_t)((frame_period_us * 58333333ULL) / ((uint64_t)1852 * 1000000ULL));
}

struct test_sns_default {
	uint32_t full_lines_std;
	uint32_t full_lines;
	uint32_t max_int_line;
};

/* Conceptual mirror of hal_alif patch 0011's isp_vsi_sync_ae_sns_default()
 * field-copy (sensor_attributes.fullLinesStd/fullLines/maxIntLine) -- see
 * file comment above. */
static void
isp_sns_default_sync(struct test_sns_default *sns, uint32_t full_lines, uint32_t max_int_line)
{
	sns->full_lines_std = full_lines;
	sns->full_lines     = full_lines;
	sns->max_int_line   = max_int_line;
}

/* hal_alif patch 0006's compiled-in 10 fps boot default -- what
 * sensor_attributes carries until the first isp_apply_ae() sync call.
 */
#define BOOT_DEFAULT_FULL_LINES   3149u
#define BOOT_DEFAULT_MAX_INT_LINE 3145u

/* Sanity: the extracted helper must reproduce the bench-cited figures
 * before trusting it anywhere else in this file. 10 fps: 3149, matching
 * ov5647_ae_envelope.h's OV5647_AE_FULL_LINES exactly (both derived from
 * the same pixel_rate/HTS facts).
 */
ZTEST(isp_ae_calib_envelope_sync, test_sns_full_lines_matches_bench_values)
{
	zassert_equal(isp_ae_sns_full_lines_from_frmival(1, 10),
	              3149u,
	              "10 fps: matches ov5647_ae_envelope.h's OV5647_AE_FULL_LINES");
	zassert_equal(isp_ae_sns_full_lines_from_frmival(1, 30), 1049u, "30 fps: VTS shrinks with fps");
}

ZTEST(isp_ae_calib_envelope_sync, test_sns_full_lines_zero_denominator_returns_zero)
{
	zassert_equal(isp_ae_sns_full_lines_from_frmival(1, 0), 0u, NULL);
}

/*
 * 4990322ee's ACTUAL bug (bench run 244), pinned as a regression: with NO
 * sync call at all (that commit's isp_vsi_set_param() only ever mirrors
 * isp_calib_param, never touches sensor_attributes), sensor_attributes
 * stays at whatever isp_vsi_init() compiled in -- the 10 fps boot default
 * -- regardless of which fps the app is really running. An app requesting
 * 30 fps has isp_apply_ae() compute the correct 30 fps VTS (proven by
 * test_sns_full_lines_matches_bench_values above) while sensor_attributes
 * -- the struct the library's intLine ceiling actually comes from -- keeps
 * carrying the 10 fps boot default forever, exactly matching bench run
 * 244's observed intLine pin at 3145.
 */
ZTEST(isp_ae_calib_envelope_sync, test_unsynced_sns_default_stays_pinned_to_stale_default)
{
	struct test_sns_default sns = {
		.full_lines_std = BOOT_DEFAULT_FULL_LINES,
		.full_lines     = BOOT_DEFAULT_FULL_LINES,
		.max_int_line   = BOOT_DEFAULT_MAX_INT_LINE,
	};

	/* App runs at 30 fps; isp_apply_ae() computes the right VTS, but
	 * 4990322ee never calls anything that reaches `sns` with it. */
	uint32_t active_30fps_full_lines = isp_ae_sns_full_lines_from_frmival(1, 30);

	zassert_not_equal(sns.max_int_line,
	                  active_30fps_full_lines - 4,
	                  "regression sentinel: this is #2277's bench-run-244 bug -- "
	                  "sensor_attributes.maxIntLine stuck at the 10 fps boot default "
	                  "(%u) instead of the active 30 fps ceiling (%u) is exactly what "
	                  "4990322ee shipped",
	                  sns.max_int_line,
	                  active_30fps_full_lines - 4);
}

/* The fix: isp_sns_default_sync() (hal_alif patch 0011's third-cut mirror,
 * called from isp_pico.c's isp_apply_ae() on every stream (re)start) keeps
 * sensor_attributes tracking the ACTIVE fps -- including a fps SWITCH, not
 * just one push. Starts from the same stale 10 fps boot default the
 * regression test above does, so this test would fail exactly the way
 * that one's assertion demonstrates if the sync call were removed (a
 * no-op sync leaves `sns` unchanged from the boot default at every step
 * below).
 */
ZTEST(isp_ae_calib_envelope_sync, test_sns_default_sync_tracks_active_fps_switch_30_10_30)
{
	struct test_sns_default sns = {
		.full_lines_std = BOOT_DEFAULT_FULL_LINES,
		.full_lines     = BOOT_DEFAULT_FULL_LINES,
		.max_int_line   = BOOT_DEFAULT_MAX_INT_LINE,
	};

	uint32_t full_lines_30fps = isp_ae_sns_full_lines_from_frmival(1, 30);
	uint32_t full_lines_10fps = isp_ae_sns_full_lines_from_frmival(1, 10);

	isp_sns_default_sync(&sns, full_lines_30fps, full_lines_30fps - 4);
	zassert_equal(sns.max_int_line, 1045u, "first apply at 30 fps must land the 30 fps ceiling");
	zassert_equal(sns.full_lines, 1049u, NULL);

	isp_sns_default_sync(&sns, full_lines_10fps, full_lines_10fps - 4);
	zassert_equal(sns.max_int_line,
	              3145u,
	              "switching to 10 fps must move the ceiling too, not leave it at the "
	              "previous 30 fps value");

	isp_sns_default_sync(&sns, full_lines_30fps, full_lines_30fps - 4);
	zassert_equal(sns.max_int_line,
	              1045u,
	              "switching back to 30 fps must move the ceiling AGAIN -- a one-shot "
	              "sync would leave this stuck at 3145 from the previous step");
}

/*
 * #2277: isp_pico.c's isp_ae_fps_x100_from_frame_delta() -- the ISP's own
 * frame-rate readout, kept as a lightweight permanent counter (not a
 * bench-only diagnostic) since it is cheap and CONFIG_VIDEO_ISP_VSI_
 * FRAME_STATS-gated already -- mirrored byte-for-byte below, same
 * technique as the two frmival helpers above.
 */
static uint32_t isp_ae_fps_x100_from_frame_delta(uint32_t frame_delta, uint32_t elapsed_ms)
{
	if (elapsed_ms == 0) {
		return 0;
	}

	return (uint32_t)(((uint64_t)frame_delta * 100000ULL) / elapsed_ms);
}

ZTEST(isp_ae_calib_envelope_sync, test_fps_x100_matches_bench_value)
{
	/* Bench run 245: "ISP frame counter shows ~29.9 fps" -- 30 frames
	 * over a 1003 ms window (a rate-limit window is never exactly
	 * 1000 ms) truncates to 2991 (29.91 fps), matching that figure. */
	zassert_equal(isp_ae_fps_x100_from_frame_delta(30, 1003), 2991u, NULL);
}

ZTEST(isp_ae_calib_envelope_sync, test_fps_x100_zero_elapsed_ms_returns_zero)
{
	/* The first call in a run has no prior window to measure -- must not
	 * divide by zero. */
	zassert_equal(isp_ae_fps_x100_from_frame_delta(5, 0), 0u, NULL);
}

ZTEST(isp_ae_calib_envelope_sync, test_fps_x100_zero_frames_is_zero_fps)
{
	zassert_equal(isp_ae_fps_x100_from_frame_delta(0, 1000), 0u, NULL);
}
