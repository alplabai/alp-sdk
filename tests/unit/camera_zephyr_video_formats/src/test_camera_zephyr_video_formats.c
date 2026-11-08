/* SPDX-License-Identifier: Apache-2.0
 *
 * #2924: the zephyr_video camera backend must REFUSE a pixel format it cannot
 * negotiate with ALP_ERR_NOSUPPORT (include/alp/camera.h) instead of keeping the
 * sensor's default layout and returning ALP_OK. The format check runs before the
 * devicetree lookup, so no camera node is needed on this build.
 */
#include <zephyr/ztest.h>

#include <alp/camera.h>
#include <alp/peripheral.h>

ZTEST_SUITE(camera_zephyr_video_formats, NULL, NULL, NULL, NULL, NULL);

static void expect_nosupport(alp_pixfmt_t fmt)
{
	alp_camera_config_t cfg = {
		.camera_id = 0,
		.width     = 640,
		.height    = 480,
		.fps       = 30,
		.format    = fmt,
	};

	zassert_is_null(alp_camera_open(&cfg), "format %d must not open", (int)fmt);
	zassert_equal(alp_last_error(),
	              ALP_ERR_NOSUPPORT,
	              "format %d: want NOSUPPORT, got %d",
	              (int)fmt,
	              (int)alp_last_error());
}

ZTEST(camera_zephyr_video_formats, test_mono_vlsb_refused)
{
	expect_nosupport(ALP_PIXFMT_MONO_VLSB);
}

ZTEST(camera_zephyr_video_formats, test_yuv420_planar_refused)
{
	expect_nosupport(ALP_PIXFMT_YUV420_PLANAR);
}

ZTEST(camera_zephyr_video_formats, test_nv12_refused)
{
	expect_nosupport(ALP_PIXFMT_NV12);
}
