/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #2278: src/backends/camera/zephyr_video.c
 * used to silently ignore alp_camera_config_t::fps.  Runs the real
 * zephyr_video backend (CONFIG_VIDEO=y links it at priority 50, see
 * zephyr/kconfigs/power-camera-display.kconfig) against three DT devices:
 *
 *   alp-camera0 -- upstream's zephyr,video-sw-generator, a REAL
 *                  frame-interval-capable video device (MIN/MAX_FRAME_RATE
 *                  1/60, default 30 -- zephyr/drivers/video/video_sw_generator.c).
 *   alp-camera1 -- this test's own alp,test-video-no-frmival fake
 *                  (test_video_no_frmival.c), which implements enough of
 *                  the video API for open()/close() to work but has NO
 *                  .set_frmival -- exercises camera_apply_fps()'s decline
 *                  path (camera_frmival.h).
 *   test_camera_err (not aliased) -- this test's alp,test-video-frmival-err
 *                  fake (test_video_frmival_err.c), whose .set_frmival
 *                  always returns -EIO -- called DIRECTLY (never through
 *                  alp_camera_open()) to exercise camera_apply_fps()'s
 *                  non-ENOSYS branches.
 *
 * ztest runs test cases in NAME order (SORT_BY_NAME), not declaration
 * order -- round 2 of this suite's review found that a rate-changing case
 * (fps_200's clamp to 60) can alphabetically precede fps_zero's, so every
 * case that touches alp-camera0's rate resets it explicitly rather than
 * assuming a fresh 30 fps default; see suite_before()/suite_after().
 *
 * native_sim / Linux-only via twister/CI (see scripts/test-all.sh); this
 * suite was additionally verified under QEMU on an ARM board (mps2/an385)
 * on the macOS dev host that cannot run native_sim's POSIX arch at all --
 * see the PR/changelog for the exact `west build ... -t run` invocation
 * and the resulting pass count.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>

#include <alp/camera.h>
#include <alp/peripheral.h>

LOG_MODULE_REGISTER(test_camera_zephyr_video, CONFIG_LOG_DEFAULT_LEVEL);

/* Reaches the shared policy directly for the branches alp_camera_open()
 * alone can't exercise cheaply (a backend-only default tolerating an
 * arbitrary video_set_frmival() error) -- same relative-path convention as
 * tests/unit/camera_registry's "../../../../src/backends/camera/camera_ops.h". */
#include "../../../../src/backends/camera/camera_frmival.h"

#define CAM0_ID 0u
#define CAM1_ID 1u
#define CAM_W   64u
#define CAM_H   64u

/* zephyr_video.c's own fallback when CONFIG_ALP_SDK_MAX_CAMERA_HANDLES
 * isn't a real Kconfig symbol (it isn't -- no Kconfig file declares it,
 * so this #define always applies): the state pool holds this many
 * handles.  The leak-regression test below deliberately exceeds it. */
#define CAMERA_STATE_POOL_SIZE 2

/* video_sw_generator.c's own DEFAULT_FRAME_RATE. */
#define SW_GENERATOR_DEFAULT_FPS 30u

static const struct device *const cam0_dev     = DEVICE_DT_GET(DT_ALIAS(alp_camera0));
static const struct device *const cam1_dev     = DEVICE_DT_GET(DT_ALIAS(alp_camera1));
static const struct device *const test_err_dev = DEVICE_DT_GET(DT_NODELABEL(test_camera_err));

/* Tracked so suite_after() can close a handle a failed zassert_* left open
 * (ztest aborts the test function on the first failed assertion, so a body
 * that opens-then-asserts-then-closes never reaches its own close() call on
 * failure) -- without this, one failing case leaks a pool slot into every
 * case that runs after it (round-2 reviewer finding). */
static alp_camera_t *current_cam;

static void suite_before(void *fixture)
{
	ARG_UNUSED(fixture);
	current_cam = NULL;

	/* Put the generator back at its own compiled-in default before
	 * EVERY case, regardless of alphabetical run order -- undoes
	 * whatever rate a previous case (e.g. fps_200's clamp to 60) left
	 * behind. */
	struct video_frmival reset = { .numerator = 1, .denominator = SW_GENERATOR_DEFAULT_FPS };
	(void)video_set_frmival(cam0_dev, &reset);
}

static void suite_after(void *fixture)
{
	ARG_UNUSED(fixture);
	if (current_cam != NULL) {
		alp_camera_close(current_cam);
		current_cam = NULL;
	}
}

static alp_camera_config_t make_cfg(uint32_t camera_id, uint8_t fps)
{
	alp_camera_config_t cfg = ALP_CAMERA_CONFIG_DEFAULT(camera_id);
	cfg.width               = CAM_W;
	cfg.height              = CAM_H;
	cfg.fps                 = fps;
	return cfg;
}

ZTEST(alp_camera_zephyr_video_fps, test_fps_zero_leaves_generator_untouched)
{
	/* Preset a rate the generator would NEVER pick on its own (its
	 * compiled-in default is 30) directly, bypassing the portable API --
	 * proves camera_apply_fps() truly left the device alone rather than
	 * merely landing back on the same value it already had (an
	 * fps=0-vs-30-coincidence the original round-1 test couldn't tell
	 * apart from "untouched"). */
	struct video_frmival preset = { .numerator = 1, .denominator = 7u };
	zassert_ok(video_set_frmival(cam0_dev, &preset), NULL);

	alp_camera_config_t cfg = make_cfg(CAM0_ID, 0u);
	current_cam             = alp_camera_open(&cfg);
	zassert_not_null(current_cam, "fps=0 must open");

	struct video_frmival fi = { 0 };
	zassert_ok(video_get_frmival(cam0_dev, &fi), NULL);
	zassert_equal(fi.numerator, 1u, NULL);
	zassert_equal(fi.denominator,
	              7u,
	              "camera_apply_fps() must not touch the device when both "
	              "requested_fps and default_fps are 0");

	alp_camera_close(current_cam);
	current_cam = NULL;
}

ZTEST(alp_camera_zephyr_video_fps, test_fps_15_settles_exactly)
{
	alp_camera_config_t cfg = make_cfg(CAM0_ID, 15u);
	current_cam             = alp_camera_open(&cfg);

	zassert_not_null(current_cam, "fps=15 is within the generator's 1-60 range");

	struct video_frmival fi = { 0 };
	zassert_ok(video_get_frmival(cam0_dev, &fi), NULL);
	zassert_equal(fi.numerator, 1u, NULL);
	zassert_equal(fi.denominator, 15u, "requested rate lands exactly");

	alp_camera_close(current_cam);
	current_cam = NULL;
}

ZTEST(alp_camera_zephyr_video_fps, test_fps_200_clamps_to_generators_ceiling)
{
	alp_camera_config_t cfg = make_cfg(CAM0_ID, 200u);
	current_cam             = alp_camera_open(&cfg);

	/* video_sw_generator_set_frmival() CLAMPs to MAX_FRAME_RATE (60) and
	 * still returns 0 -- not a decline, just a nearest-settle -- so
	 * open() succeeds instead of failing with ALP_ERR_NOSUPPORT. */
	zassert_not_null(current_cam, "an out-of-range request settles, it doesn't fail open()");

	struct video_frmival fi = { 0 };
	zassert_ok(video_get_frmival(cam0_dev, &fi), NULL);
	zassert_equal(fi.numerator, 1u, NULL);
	zassert_equal(fi.denominator, 60u, "clamped to the generator's own ceiling");

	alp_camera_close(current_cam);
	current_cam = NULL;
}

ZTEST(alp_camera_zephyr_video_fps, test_fps_nonzero_on_no_frmival_device_declines_loudly)
{
	alp_camera_config_t cfg = make_cfg(CAM1_ID, 30u);
	alp_camera_t       *cam = alp_camera_open(&cfg);

	zassert_is_null(cam, "alp-camera1 has no frame-rate control at all");
	zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_camera_zephyr_video_fps, test_fps_zero_on_no_frmival_device_opens)
{
	alp_camera_config_t cfg = make_cfg(CAM1_ID, 0u);
	current_cam             = alp_camera_open(&cfg);

	zassert_not_null(current_cam,
	                 "fps=0 has no opinion, so the missing frmival ops are never touched");

	/* No frame-interval getter exists on this fake (deliberately -- see
	 * test_video_no_frmival.c); video_get_frmival() itself must report
	 * the absence rather than crash. */
	struct video_frmival fi = { 0 };
	zassert_equal(video_get_frmival(cam1_dev, &fi), -ENOSYS);

	alp_camera_close(current_cam);
	current_cam = NULL;
}

ZTEST(alp_camera_zephyr_video_fps, test_fps_decline_does_not_leak_the_state_pool)
{
	/* A single decline-then-reopen can't tell a REAL slot release apart
	 * from a pool that merely had one spare slot to begin with -- decline
	 * pool_size+1 times in a row so a leaked _free_state() would exhaust
	 * the pool before the final reopen (round-2 reviewer finding: the
	 * original single-decline test still passed 5/5 under a mutation that
	 * deleted zephyr_video.c's _free_state() call on this path). */
	for (int i = 0; i < CAMERA_STATE_POOL_SIZE + 1; i++) {
		alp_camera_config_t cfg = make_cfg(CAM1_ID, 30u);
		alp_camera_t       *cam = alp_camera_open(&cfg);

		zassert_is_null(cam, "decline #%d must not hand back a handle", i);
	}

	alp_camera_config_t retry = make_cfg(CAM1_ID, 0u);
	current_cam               = alp_camera_open(&retry);

	zassert_not_null(current_cam,
	                 "%d declines against a %d-slot pool must not exhaust it -- each decline "
	                 "releases its slot before returning",
	                 CAMERA_STATE_POOL_SIZE + 1,
	                 CAMERA_STATE_POOL_SIZE);

	alp_camera_close(current_cam);
	current_cam = NULL;
}

ZTEST(alp_camera_zephyr_video_fps, test_apply_fps_backend_default_tolerates_enosys)
{
	/* Direct camera_apply_fps() call: requested_fps == 0 (only a backend
	 * default asked), default_fps == 10, against alp-camera1 which has
	 * no frame-rate control at all (-ENOSYS).  Must not fail -- a
	 * backend-only default never bricks an open() the caller never asked
	 * to fail (restores alif_isp_pico.c's pre-#2278 tolerance). */
	struct video_frmival settled = { .numerator = 111, .denominator = 222 }; /* poison */
	alp_status_t         status  = camera_apply_fps(cam1_dev, CAM1_ID, 0u, 10u, &settled);

	zassert_equal(status, ALP_OK, "a backend-only default must tolerate -ENOSYS, not fail open()");
	zassert_equal(settled.numerator, 0u, "no successful set landed -- settled stays {0,0}");
	zassert_equal(settled.denominator, 0u, NULL);
}

ZTEST(alp_camera_zephyr_video_fps, test_apply_fps_backend_default_tolerates_non_enosys_error)
{
	/* Same as above, but against test_camera_err, whose .set_frmival
	 * always returns -EIO -- a non-ENOSYS error.  The policy is graded
	 * by WHO asked (requested_fps == 0 here), not by the errno, so this
	 * must ALSO just warn and return ALP_OK. */
	struct video_frmival settled = { .numerator = 111, .denominator = 222 };
	alp_status_t         status  = camera_apply_fps(test_err_dev, CAM1_ID, 0u, 10u, &settled);

	zassert_equal(status, ALP_OK, "a backend-only default must tolerate ANY set_frmival() error");
	zassert_equal(settled.numerator, 0u, NULL);
	zassert_equal(settled.denominator, 0u, NULL);
}

ZTEST(alp_camera_zephyr_video_fps, test_apply_fps_caller_non_enosys_error_maps_through)
{
	/* A CALLER-requested nonzero rate against the same -EIO device must
	 * NOT decline with ALP_ERR_NOSUPPORT (that status is reserved for
	 * -ENOSYS/-ENOTSUP) -- it maps through the shared errno baseline
	 * instead, same as any other backend call failure. */
	struct video_frmival settled = { 0 };
	alp_status_t         status  = camera_apply_fps(test_err_dev, CAM1_ID, 15u, 0u, &settled);

	zassert_equal(status,
	              ALP_ERR_IO,
	              "a non-ENOSYS video_set_frmival() failure on a CALLER request maps "
	              "through alp_status_from_zephyr_errno(), it does not become "
	              "ALP_ERR_NOSUPPORT");
}

ZTEST_SUITE(alp_camera_zephyr_video_fps, NULL, NULL, suite_before, suite_after, NULL);
