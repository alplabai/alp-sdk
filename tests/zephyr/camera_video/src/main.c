/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #1628: proves src/backends/camera/zephyr_video.c both compiles and
 * behaves correctly against the pinned Zephyr v4.4.1 drivers/video/ API.
 *
 * Part 1 (compile + basic operation): no other in-repo CONFIG_VIDEO=y
 * scenario also sets CONFIG_ALP_SDK_CAMERA_ZEPHYR_VIDEO=y, so this is the
 * only build that actually compiles the file -- see prj.conf.  camera0
 * (backed by the upstream zephyr,video-sw-generator, which fills
 * video_format.pitch on set_format) round-trips open/start/capture/
 * release/stop/close through the ported v4.4 API.
 *
 * Part 2 (buffer sizing): camera1 (backed by this test's own
 * alp,fake-video-pitch0 fixture, which never fills pitch) proves open()
 * derives bytes-per-pixel from the negotiated fourcc instead of assuming
 * a flat 2 B/px -- the fixture's enqueue hook refuses any video_buffer
 * smaller than width * height * 3 bytes for RGB888, reproducing the
 * CSI-2/ISP DMA overrun the old flat-2-B/px sizing would have caused.
 *
 * Part 3 (JPEG / compressed-format sizing, review follow-up): camera2
 * (backed by this test's own alp,fake-video-jpeg0 fixture, which reports
 * VIDEO_PIX_FMT_JPEG with pitch == 0) proves open() falls back to the
 * flat w*h*2 heuristic bound for a fourcc video_bits_per_pixel() can't
 * size at all, instead of hard-refusing with ALP_ERR_NOSUPPORT.
 */

#include <zephyr/ztest.h>

#include <alp/camera.h>

ZTEST(camera_video, test_zephyr_video_open_start_capture_close_roundtrip)
{
	alp_camera_config_t cfg = ALP_CAMERA_CONFIG_DEFAULT(0);

	cfg.width  = 320;
	cfg.height = 160;
	cfg.format = ALP_PIXFMT_RGB565;

	alp_camera_t *cam = alp_camera_open(&cfg);

	zassert_not_null(cam, "camera0 open failed: %d", alp_last_error());

	zassert_equal(alp_camera_start(cam), ALP_OK);

	alp_camera_frame_t frame  = { 0 };
	alp_status_t       status = alp_camera_capture(cam, &frame, 1000);

	zassert_equal(status, ALP_OK, "capture failed: %d", status);
	zassert_not_null(frame.data);
	zassert_true(frame.size > 0, "captured frame reported zero size");

	zassert_equal(alp_camera_release(cam, &frame), ALP_OK);
	zassert_equal(alp_camera_stop(cam), ALP_OK);
	alp_camera_close(cam);
}

ZTEST(camera_video, test_zephyr_video_pitch_zero_sizes_by_fourcc_not_flat_2bpp)
{
	alp_camera_config_t cfg = ALP_CAMERA_CONFIG_DEFAULT(1);

	cfg.width  = 64;
	cfg.height = 48;
	cfg.format = ALP_PIXFMT_RGB888;

	alp_camera_t *cam = alp_camera_open(&cfg);

	zassert_not_null(
	    cam, "camera1 open failed -- buffer under-sized for RGB888? err=%d", alp_last_error());

	alp_camera_close(cam);
}

ZTEST(camera_video, test_zephyr_video_jpeg_pitch_zero_falls_back_to_flat_heuristic)
{
	alp_camera_config_t cfg = ALP_CAMERA_CONFIG_DEFAULT(2);

	/* ALP_PIXFMT_MONO_VLSB is the enum's zero value: _to_video_fourcc()
	 * maps it to 0, so open() takes the readback branch and negotiates
	 * nothing -- it just reads back camera2's already-JPEG default via
	 * video_get_format(). */
	cfg.format = ALP_PIXFMT_MONO_VLSB;

	alp_camera_t *cam = alp_camera_open(&cfg);

	zassert_not_null(cam, "camera2 (JPEG default, pitch==0) open failed: %d", alp_last_error());

	alp_camera_close(cam);
}

ZTEST_SUITE(camera_video, NULL, NULL, NULL, NULL, NULL);
