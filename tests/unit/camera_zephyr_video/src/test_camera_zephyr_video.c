/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #2278: src/backends/camera/zephyr_video.c
 * used to silently ignore alp_camera_config_t::fps.  Runs the real
 * zephyr_video backend (CONFIG_VIDEO=y links it at priority 50, see
 * zephyr/kconfigs/power-camera-display.kconfig) against two DT devices:
 *
 *   alp-camera0 -- upstream's zephyr,video-sw-generator, a REAL
 *                  frame-interval-capable video device (MIN/MAX_FRAME_RATE
 *                  1/60, default 30 -- zephyr/drivers/video/video_sw_generator.c).
 *   alp-camera1 -- this test's own alp,test-video-no-frmival fake
 *                  (test_video_no_frmival.c), which implements enough of
 *                  the video API for open()/close() to work but has NO
 *                  .set_frmival -- exercises camera_apply_fps()'s decline
 *                  path (camera_frmival.h).
 *
 * native_sim / Linux-only: not run on this macOS dev host (twister needs
 * the Zephyr SDK's native_sim toolchain); exercised in CI via
 * scripts/test-all.sh's twister pass.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/ztest.h>

#include <alp/camera.h>
#include <alp/peripheral.h>

#define CAM0_ID 0u
#define CAM1_ID 1u
#define CAM_W   64u
#define CAM_H   64u

static const struct device *const cam0_dev = DEVICE_DT_GET(DT_ALIAS(alp_camera0));
static const struct device *const cam1_dev = DEVICE_DT_GET(DT_ALIAS(alp_camera1));

static alp_camera_config_t make_cfg(uint32_t camera_id, uint8_t fps)
{
	alp_camera_config_t cfg = ALP_CAMERA_CONFIG_DEFAULT(camera_id);
	cfg.width               = CAM_W;
	cfg.height              = CAM_H;
	cfg.fps                 = fps;
	return cfg;
}

ZTEST(alp_camera_zephyr_video_fps, test_fps_zero_leaves_generator_at_its_own_default)
{
	alp_camera_config_t cfg = make_cfg(CAM0_ID, 0u);
	alp_camera_t       *cam = alp_camera_open(&cfg);

	zassert_not_null(cam, "fps=0 must open");

	struct video_frmival fi = { 0 };
	zassert_ok(video_get_frmival(cam0_dev, &fi), NULL);
	/* video_sw_generator.c's DEFAULT_FRAME_RATE -- untouched because
	 * camera_apply_fps() never calls video_set_frmival() when both
	 * requested_fps and default_fps (zephyr_video.c passes 0) are 0. */
	zassert_equal(fi.numerator, 1u, NULL);
	zassert_equal(fi.denominator, 30u, "generator's own default, left alone");

	alp_camera_close(cam);
}

ZTEST(alp_camera_zephyr_video_fps, test_fps_15_settles_exactly)
{
	alp_camera_config_t cfg = make_cfg(CAM0_ID, 15u);
	alp_camera_t       *cam = alp_camera_open(&cfg);

	zassert_not_null(cam, "fps=15 is within the generator's 1-60 range");

	struct video_frmival fi = { 0 };
	zassert_ok(video_get_frmival(cam0_dev, &fi), NULL);
	zassert_equal(fi.numerator, 1u, NULL);
	zassert_equal(fi.denominator, 15u, "requested rate lands exactly");

	alp_camera_close(cam);
}

ZTEST(alp_camera_zephyr_video_fps, test_fps_200_clamps_to_generators_ceiling)
{
	alp_camera_config_t cfg = make_cfg(CAM0_ID, 200u);
	alp_camera_t       *cam = alp_camera_open(&cfg);

	/* video_sw_generator_set_frmival() CLAMPs to MAX_FRAME_RATE (60) and
	 * still returns 0 -- not a decline, just a nearest-settle -- so
	 * open() succeeds instead of failing with ALP_ERR_NOSUPPORT. */
	zassert_not_null(cam, "an out-of-range request settles, it doesn't fail open()");

	struct video_frmival fi = { 0 };
	zassert_ok(video_get_frmival(cam0_dev, &fi), NULL);
	zassert_equal(fi.numerator, 1u, NULL);
	zassert_equal(fi.denominator, 60u, "clamped to the generator's own ceiling");

	alp_camera_close(cam);
}

ZTEST(alp_camera_zephyr_video_fps, test_fps_nonzero_on_no_frmival_device_declines_loudly)
{
	alp_camera_config_t cfg = make_cfg(CAM1_ID, 30u);
	alp_camera_t       *cam = alp_camera_open(&cfg);

	zassert_is_null(cam, "alp-camera1 has no frame-rate control at all");
	zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);

	/* Re-open the SAME camera_id at fps=0 right after the decline --
	 * proves the backend's state-pool slot was released on the failed
	 * open (no leak, mirrors issue #246's pattern) rather than staying
	 * claimed forever. */
	alp_camera_config_t retry = make_cfg(CAM1_ID, 0u);
	alp_camera_t       *cam2  = alp_camera_open(&retry);

	zassert_not_null(cam2, "fps=0 never touches the device, so open() must succeed");
	alp_camera_close(cam2);
}

ZTEST(alp_camera_zephyr_video_fps, test_fps_zero_on_no_frmival_device_opens)
{
	alp_camera_config_t cfg = make_cfg(CAM1_ID, 0u);
	alp_camera_t       *cam = alp_camera_open(&cfg);

	zassert_not_null(cam, "fps=0 has no opinion, so the missing frmival ops are never touched");

	/* No frame-interval getter exists on this fake (deliberately -- see
	 * test_video_no_frmival.c); video_get_frmival() itself must report
	 * the absence rather than crash. */
	struct video_frmival fi = { 0 };
	zassert_equal(video_get_frmival(cam1_dev, &fi), -ENOSYS);

	alp_camera_close(cam);
}

ZTEST_SUITE(alp_camera_zephyr_video_fps, NULL, NULL, NULL, NULL, NULL);
