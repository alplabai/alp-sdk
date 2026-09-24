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
 * Keep the two in sync by eye if either changes; there is no #include path
 * from this alp-sdk-owned test to the vendored patch to enforce it
 * mechanically without inverting the vendor/alp-sdk layering direction.
 *
 * Without the clamp, E1M-AEN803 + OV5647 bench traffic showed AE settle on
 * a gain writeback of 0x2000 (8192) against OV5647's registered
 * VIDEO_CID_ANALOGUE_GAIN range of [0, 1023] -- video_set_ctrl() rejected
 * every write with -EINVAL, logging "Control value is invalid" once per
 * frame forever (cached_sns_config only updates on success, so it kept
 * retrying the same rejected value). test_clamp_regresses_the_bench_value
 * below pins exactly that case.
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

/* The regression case: bench run computed a gain writeback of 0x2000 against
 * OV5647's registered [0, 1023] -- without the clamp this reaches
 * video_set_ctrl() out of range and gets rejected with -EINVAL every frame.
 */
ZTEST(isp_ae_ctrl_clamp, test_clamp_regresses_the_bench_value)
{
	zassert_equal(isp_clamp_ctrl_val(0x2000, 0, 1023),
	              1023,
	              "0x2000 must saturate to OV5647's registered max, not pass through");
}
