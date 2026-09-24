/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-side (native_sim) regression for the AE-writeback range clamp added
 * by zephyr/patches/hal_alif/0010-isp-clamp-ae-writeback-to-ctrl-range.patch
 * (isp_clamp_ctrl_val(), drivers/isp/isp_wrapper/src/isp_api_wrapper.c in
 * the hal_alif module).
 *
 * That module is vendored, not part of this build, and isp_vsi_bottom_half()
 * itself is not host-buildable -- it calls into the closed VSI_MPI_ISP_*
 * ISP library, which exists only for the real target. isp_clamp_ctrl_val()
 * is a dependency-free 3-branch int32_t clamp with no such dependency, so
 * this file mirrors it VERBATIM (see the copy below) and tests the copy.
 * Keep the two in sync if either changes -- there is no #include path from
 * this alp-sdk-owned test to the vendored patch to enforce it mechanically
 * without inverting the vendor/alp-sdk layering direction, so
 * tests/scripts/test_isp_ae_ctrl_clamp_patch_mirror.py does it from outside
 * instead: it extracts both copies' function bodies and fails the moment
 * they stop matching (whitespace aside).
 *
 * Without the clamp, E1M-AEN803 + OV5647 bench traffic (runs 211/212) showed
 * AE settle on an EXPOSURE writeback of intLine 94470 (0x171E6) -- ctrl.val =
 * intLine * 16 = 1511520 -- against OV5647's registered VIDEO_CID_EXPOSURE
 * range of [0, 0xFFFFF] (1048575, GENMASK(19,0) in ov5647.c):
 * video_set_ctrl() rejected every write with -EINVAL, logging "Control value
 * is invalid" once per frame forever (cached_sns_config only updates on
 * success, so it kept retrying the same rejected value). The GAIN writeback
 * that same bench run (aGain 0x2000, dGain 0x9cd) computed ctrl.val = 313 --
 * inside OV5647's registered VIDEO_CID_ANALOGUE_GAIN range of [0, 1023], not
 * the violator; an earlier revision of this file mis-cited the gain value as
 * the out-of-range one (#2271's actual root cause: the ISP library's own
 * compiled-in AE calibration block, isp_param_conf.h, not this clamp's own
 * target range -- see hal_alif patch 0011). test_clamp_regresses_the_bench_
 * value below pins the real (exposure) case.
 */
#include <stdint.h>

#include <zephyr/ztest.h>

/* Mirror of isp_clamp_ctrl_val() from the 0010 patch -- see file comment. */
static inline int32_t isp_clamp_ctrl_val(int32_t val, int32_t min, int32_t max)
{
	if (val < min) {
		return min;
	}
	if (val > max) {
		return max;
	}
	return val;
}

ZTEST_SUITE(isp_ae_ctrl_clamp, NULL, NULL, NULL, NULL, NULL);

ZTEST(isp_ae_ctrl_clamp, test_in_range_value_passes_through_unchanged)
{
	zassert_equal(isp_clamp_ctrl_val(500, 0, 1023), 500, NULL);
}

ZTEST(isp_ae_ctrl_clamp, test_below_min_clamps_to_min)
{
	zassert_equal(isp_clamp_ctrl_val(-5, 0, 1023), 0, NULL);
}

ZTEST(isp_ae_ctrl_clamp, test_exact_boundaries_pass_through)
{
	zassert_equal(isp_clamp_ctrl_val(0, 0, 1023), 0, "min boundary is inclusive");
	zassert_equal(isp_clamp_ctrl_val(1023, 0, 1023), 1023, "max boundary is inclusive");
}

/* The regression case: bench run 211/212 computed an EXPOSURE writeback of
 * 1511520 (intLine 94470 * 16) against OV5647's registered [0, 0xFFFFF] --
 * without the clamp this reaches video_set_ctrl() out of range and gets
 * rejected with -EINVAL every frame.
 */
ZTEST(isp_ae_ctrl_clamp, test_clamp_regresses_the_bench_value)
{
	zassert_equal(isp_clamp_ctrl_val(1511520, 0, 0xFFFFF),
	              0xFFFFF,
	              "1511520 must saturate to OV5647's registered exposure max, not pass "
	              "through");
}
