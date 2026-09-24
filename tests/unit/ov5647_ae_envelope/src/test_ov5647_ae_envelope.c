/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host-side (native_sim) regression for the fps-parameterized AE exposure
 * envelope added by zephyr/patches/hal_alif/0011-isp-ov5647-ae-calib-envelope
 * .patch (ov5647_ae_envelope.h, new in that patch, drivers/isp/isp_wrapper/
 * inc/ in the hal_alif module).
 *
 * That module is vendored, not part of this build -- ov5647_ae_envelope.h
 * pulls in vsios_type.h (also vendored), and its consumers (isp_param_conf.h,
 * sensor_attributes.h) are not host-buildable. OV5647_AE_PIXEL_RATE_HZ /
 * OV5647_AE_HTS_640X480 / OV5647_AE_VTS_AT_FPS() / OV5647_AE_MAX_INT_LINE_AT_
 * FPS() have no such dependency (plain integer arithmetic on a locally
 * typedef'd vsi_u32_t, see below), so this file mirrors them VERBATIM (see
 * the copy below) and tests the copy. Keep the two in sync if either
 * changes -- there is no #include path from this alp-sdk-owned test to
 * the vendored patch that would keep the two mechanically identical
 * (and inverting that dependency would put alp-sdk-owned test code
 * including a vendor patch, the wrong direction), so
 * tests/scripts/test_ov5647_ae_envelope_patch_mirror.py does it from
 * outside instead, the same way test_isp_ae_ctrl_clamp_patch_mirror.py
 * backs isp_ae_ctrl_clamp (#2271, tests/unit/isp_ae_ctrl_clamp).
 *
 * Issue #2277: OV5647_AE_MAX_INT_LINE used to be a single #define pinned to
 * the sensor's 10 fps VTS (3145 lines) regardless of the ACTIVE requested
 * rate -- at 30 fps (VTS 1049 lines, <alp/camera.h>'s
 * ALP_CAMERA_CONFIG_DEFAULT) or 60 fps (VTS 524 lines) that left AE free to
 * command an exposure far past the mode's own frame period, stretching the
 * frame and dropping the effective capture rate in a dim scene instead of
 * holding it. test_pinned_10fps_ceiling_violates_faster_modes below pins
 * exactly that bug (a regression guard against reintroducing it);
 * test_max_int_line_stays_under_frame_period_at_*fps prove the fix.
 */
#include <stdint.h>

#include <zephyr/ztest.h>

/* vsios_type.h's own typedef (vendored, not part of this build) -- kept
 * under the SAME name so the macro line below stays byte-for-byte
 * identical to the patch's, not just numerically equivalent.
 */
typedef uint32_t vsi_u32_t;

/* Mirror of ov5647_ae_envelope.h's fps-parameterized ceiling -- see file
 * comment.
 */
#define OV5647_AE_PIXEL_RATE_HZ 58333333u /* ov5647.c's OV5647_PIXEL_RATE() at 25 MHz XVCLK */
#define OV5647_AE_HTS_640X480   1852u     /* ov5647.c's OV5647_HTS_640X480_BINNED (0x073c) */

#define OV5647_AE_VTS_AT_FPS(fps) \
	(OV5647_AE_PIXEL_RATE_HZ / (OV5647_AE_HTS_640X480 * (vsi_u32_t)(fps)))

#define OV5647_AE_MAX_INT_LINE_AT_FPS(fps) (OV5647_AE_VTS_AT_FPS(fps) - 4)

/* The OLD (pre-#2277) behaviour: one ceiling, always the 10 fps VTS - 4,
 * regardless of which fps is actually active -- kept here, not in the
 * mirror above, purely so the regression test below can show what the bug
 * looked like.
 */
#define OV5647_AE_MAX_INT_LINE_PINNED_TO_10FPS 3145u

ZTEST_SUITE(ov5647_ae_envelope, NULL, NULL, NULL, NULL, NULL);

/* VTS sanity: the formula must reproduce the bench-matched 10 fps figure
 * (3149 lines, sensor_attributes.h's .fullLines / patch 0006's own
 * derivation comment) before trusting it at any other rate.
 */
ZTEST(ov5647_ae_envelope, test_vts_matches_bench_value_at_10fps)
{
	zassert_equal(OV5647_AE_VTS_AT_FPS(10), 3149u, NULL);
}

/* The regression: at each of the OV5647's supported rates this driver
 * actually reaches in its 640x480 binned mode (10/30/60 fps), AE's
 * commanded exposure ceiling must stay under THAT mode's own VTS (with the
 * same 4-line margin the fixed constant it replaces used) -- never a
 * different mode's, looser ceiling.
 */
ZTEST(ov5647_ae_envelope, test_max_int_line_stays_under_frame_period_at_10fps)
{
	uint32_t vts = OV5647_AE_VTS_AT_FPS(10);

	zassert_true(OV5647_AE_MAX_INT_LINE_AT_FPS(10) <= vts - 4,
	             "10 fps ceiling %u must not exceed VTS %u - 4 margin",
	             OV5647_AE_MAX_INT_LINE_AT_FPS(10),
	             vts);
}

ZTEST(ov5647_ae_envelope, test_max_int_line_stays_under_frame_period_at_30fps)
{
	uint32_t vts = OV5647_AE_VTS_AT_FPS(30);

	zassert_true(OV5647_AE_MAX_INT_LINE_AT_FPS(30) <= vts - 4,
	             "30 fps ceiling %u must not exceed VTS %u - 4 margin",
	             OV5647_AE_MAX_INT_LINE_AT_FPS(30),
	             vts);
}

ZTEST(ov5647_ae_envelope, test_max_int_line_stays_under_frame_period_at_60fps)
{
	uint32_t vts = OV5647_AE_VTS_AT_FPS(60);

	zassert_true(OV5647_AE_MAX_INT_LINE_AT_FPS(60) <= vts - 4,
	             "60 fps ceiling %u must not exceed VTS %u - 4 margin",
	             OV5647_AE_MAX_INT_LINE_AT_FPS(60),
	             vts);
}

/* Proves the fix actually changes behaviour: the OLD pinned-to-10fps
 * ceiling violates the 30 fps and 60 fps frame periods -- with the old
 * single #define, AE could still legally command up to 3145 lines' worth
 * of exposure at 30 or 60 fps, stretching those modes' much shorter frame
 * periods and dropping the effective capture rate in a dim scene. This is
 * the "fails without the fix" case: swap OV5647_AE_MAX_INT_LINE_AT_FPS()
 * above for the pinned constant everywhere and
 * test_max_int_line_stays_under_frame_period_at_30fps/_60fps fail.
 */
ZTEST(ov5647_ae_envelope, test_pinned_10fps_ceiling_violates_faster_modes)
{
	zassert_true(OV5647_AE_MAX_INT_LINE_PINNED_TO_10FPS > OV5647_AE_VTS_AT_FPS(30),
	             "sanity: the bug this fix replaces let AE ask for more lines than a "
	             "30 fps frame even holds (pinned=%u, 30fps VTS=%u)",
	             OV5647_AE_MAX_INT_LINE_PINNED_TO_10FPS,
	             OV5647_AE_VTS_AT_FPS(30));
	zassert_true(OV5647_AE_MAX_INT_LINE_PINNED_TO_10FPS > OV5647_AE_VTS_AT_FPS(60),
	             "sanity: the bug this fix replaces let AE ask for more lines than a "
	             "60 fps frame even holds (pinned=%u, 60fps VTS=%u)",
	             OV5647_AE_MAX_INT_LINE_PINNED_TO_10FPS,
	             OV5647_AE_VTS_AT_FPS(60));
}
