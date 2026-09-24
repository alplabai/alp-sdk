/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-side (native_sim) regression for #2277's REAL fix: the OV5647 AE
 * calibration's exposure/gain envelope must track the ACTIVE fps at
 * runtime, not a value guessed at compile time.
 *
 * The first cut of #2277 made isp_param_conf.h's compiled-in ceiling a
 * function of a Kconfig-selected fps (VIDEO_ISP_VSI_CALIB_OV5647_FPS,
 * default 30) instead of a hardcoded 10 -- still ONE compile-time value, so
 * an app that actually runs at a DIFFERENT fps (this backend's own 10 fps
 * fallback, both examples/aen/aen-isp-ov5647-* apps) still got the wrong
 * ceiling. A macro-only test of that formula (deleted alongside it) could
 * only ever prove the arithmetic self-consistent, never that the ceiling
 * actually reaching the ISP library tracks what the app is DOING at
 * runtime -- exactly the gap the fix-first review round on #2277 caught.
 *
 * This file mirrors the two pieces of the REAL runtime path instead:
 *
 *   1. isp_ae_int_time_max_us_from_frmival() -- zephyr/drivers/video/
 *      isp_pico.c's own pure helper (pulled out of isp_apply_ae() by this
 *      same #2277 change so it CAN be mirrored here): the sensor's ACTIVE
 *      frame interval (video_get_frmival(), sensor-agnostic) in, an
 *      exposure-time ceiling in microseconds out. isp_pico.c is alp-sdk-
 *      owned (not a vendored hal_alif patch), so
 *      tests/scripts/test_isp_ae_calib_envelope_sync_patch_mirror.py
 *      extracts this function's body directly from that file (brace-
 *      matched, the same technique test_isp_ae_ctrl_clamp_patch_mirror.py
 *      uses against a vendored patch) and fails if the two drift.
 *
 *   2. isp_calib_ae_envelope_sync() -- a conceptual mirror of the three
 *      field-copy lines zephyr/patches/hal_alif/0011-isp-ov5647-ae-calib-
 *      envelope.patch adds to isp_api_wrapper.c's isp_vsi_set_param(): it
 *      mirrors ISP_EXPOSURE_ATTR_S.autoAttr's expTimeRange/againRange/
 *      dgainRange from the struct isp_apply_ae() just pushed into
 *      isp_calib_param.modules.ae, the SAME struct VSI_MPI_ISP_SetCalib()
 *      (isp_vsi_init(), isp_api_wrapper.c) reprograms the library from on
 *      any later reload. isp_api_wrapper.c is vendored (not host-
 *      buildable -- it calls into the closed VSI_MPI_ISP library and needs
 *      vendor struct types this file cannot include), so this is a
 *      minimal-POD-type mirror of the SAME three field paths, not the
 *      literal vendor-typed function; the patch-mirror script checks the
 *      field-path SET (autoAttr.{expTimeRange,againRange,dgainRange}),
 *      not byte-identical text, for exactly that reason -- see the script's
 *      own comment.
 *
 * test_unsynced_calib_stays_pinned_to_stale_default below pins e263820a8's
 * ACTUAL bug: with no sync call at all (that commit's isp_vsi_set_param()
 * only calls VSI_MPI_ISP_SetExposureAttr(), never touches isp_calib_param),
 * isp_calib_param stays at whatever isp_vsi_init() compiled in -- wrong for
 * any app whose active fps differs from that compile-time default.
 * test_calib_envelope_sync_tracks_active_fps_switch_30_10_30 proves the fix:
 * every isp_apply_ae() call re-syncs isp_calib_param to the ACTIVE fps,
 * including switching back and forth, never staying stuck on a stale value.
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

/* Minimal POD mirror of ISP_AE_RANGE_S/ISP_EXPOSURE_ATTR_S's relevant shape
 * -- see file comment for why a literal vendor-typed mirror is not
 * possible here.
 */
struct test_ae_range {
	uint32_t min;
	uint32_t max;
};

struct test_ae_auto_attr {
	struct test_ae_range exp_time_range;
	struct test_ae_range again_range;
	struct test_ae_range dgain_range;
};

/* Conceptual mirror of hal_alif patch 0011's isp_calib_ae_envelope_sync
 * field-copy -- see file comment. */
static void isp_calib_ae_envelope_sync(struct test_ae_auto_attr       *calib_ae,
                                       const struct test_ae_auto_attr *pushed_ae)
{
	calib_ae->exp_time_range = pushed_ae->exp_time_range;
	calib_ae->again_range    = pushed_ae->again_range;
	calib_ae->dgain_range    = pushed_ae->dgain_range;
}

/* isp_calib_param's compiled-in boot-time default (ov5647_ae_envelope.h,
 * OV5647_AE_EXP_TIME_MAX_US at the 10 fps default: 3145 lines * 1000000 /
 * (3149 * 10) = 99872 us) -- what isp_vsi_init()'s SetCalib() loads before
 * any app has opened the camera and negotiated a real fps.
 */
#define BOOT_DEFAULT_EXP_TIME_MAX_US 99872u

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

/* e263820a8's ACTUAL bug, pinned as a regression: with NO sync call
 * (isp_vsi_set_param() only ever called VSI_MPI_ISP_SetExposureAttr(),
 * never touched isp_calib_param), the calibration's envelope stays at
 * whatever isp_vsi_init() compiled in -- 10 fps's ceiling in this repo's
 * actual default build (ov5647_ae_envelope.h) -- regardless of which fps
 * the app is really running. An app that requests 30 fps (this SDK's own
 * ALP_CAMERA_CONFIG_DEFAULT) would, on e263820a8, have isp_apply_ae() push
 * the CORRECT 30 fps ceiling to isp_vsi_set_param() (proven by
 * test_int_time_max_us_matches_bench_values above) while isp_calib_param
 * -- the struct SetCalib() actually reprograms the library from on any
 * later reload -- keeps carrying the 10 fps boot default forever.
 */
ZTEST(isp_ae_calib_envelope_sync, test_unsynced_calib_stays_pinned_to_stale_default)
{
	struct test_ae_auto_attr calib = {
		.exp_time_range = { .min = 32, .max = BOOT_DEFAULT_EXP_TIME_MAX_US },
	};

	/* App runs at 30 fps; isp_apply_ae() computes the right ceiling, but
	 * e263820a8 never calls anything that reaches `calib` with it. */
	uint32_t active_30fps_ceiling = isp_ae_int_time_max_us_from_frmival(1, 30);

	zassert_not_equal(calib.exp_time_range.max,
	                  active_30fps_ceiling,
	                  "regression sentinel: this is the #2277 bug -- calib stuck at "
	                  "the 10 fps default (%u) instead of the active 30 fps ceiling "
	                  "(%u) is exactly what e263820a8 shipped",
	                  calib.exp_time_range.max,
	                  active_30fps_ceiling);
}

/* The fix: isp_calib_ae_envelope_sync() (hal_alif patch 0011's mirror,
 * called from isp_vsi_set_param() on every isp_apply_ae() apply) keeps
 * isp_calib_param's envelope tracking the ACTIVE fps -- including a
 * fps SWITCH, not just one push. Starts from the same stale 10 fps boot
 * default test_unsynced_calib_stays_pinned_to_stale_default does, so this
 * test would fail exactly the way that one's assertion demonstrates if the
 * sync call were removed (a no-op sync leaves `calib` unchanged from the
 * boot default at every step below).
 */
ZTEST(isp_ae_calib_envelope_sync, test_calib_envelope_sync_tracks_active_fps_switch_30_10_30)
{
	struct test_ae_auto_attr calib = {
		.exp_time_range = { .min = 32, .max = BOOT_DEFAULT_EXP_TIME_MAX_US },
	};

	struct test_ae_auto_attr pushed_30fps = {
		.exp_time_range = { .min = 32, .max = isp_ae_int_time_max_us_from_frmival(1, 30) },
	};
	struct test_ae_auto_attr pushed_10fps = {
		.exp_time_range = { .min = 32, .max = isp_ae_int_time_max_us_from_frmival(1, 10) },
	};

	isp_calib_ae_envelope_sync(&calib, &pushed_30fps);
	zassert_equal(
	    calib.exp_time_range.max, 32667u, "first apply at 30 fps must land the 30 fps ceiling");

	isp_calib_ae_envelope_sync(&calib, &pushed_10fps);
	zassert_equal(calib.exp_time_range.max,
	              98000u,
	              "switching to 10 fps must move the calib ceiling too, not leave it "
	              "at the previous 30 fps value");

	isp_calib_ae_envelope_sync(&calib, &pushed_30fps);
	zassert_equal(calib.exp_time_range.max,
	              32667u,
	              "switching back to 30 fps must move the calib ceiling AGAIN -- a "
	              "one-shot sync (e.g. only on the first apply) would leave this "
	              "stuck at 98000 from the previous step");
}

/* The sync mirrors the GAIN range too, not just exposure time -- again/
 * dgain ranges are pushed by the same isp_apply_ae() call and read back by
 * the same struct.
 */
ZTEST(isp_ae_calib_envelope_sync, test_calib_envelope_sync_mirrors_gain_ranges)
{
	struct test_ae_auto_attr calib = {
		.again_range = { .min = 1024, .max = 8 * 1024 }, /* ARX3A0 stock calib range */
		.dgain_range = { .min = 1024, .max = 8 * 1024 },
	};
	struct test_ae_auto_attr pushed = {
		.again_range = { .min = 1024, .max = 65472 }, /* OV5647's real ceiling */
		.dgain_range = { .min = 1024, .max = 1024 },
	};

	isp_calib_ae_envelope_sync(&calib, &pushed);

	zassert_equal(calib.again_range.max, 65472u, NULL);
	zassert_equal(calib.dgain_range.max, 1024u, NULL);
}
