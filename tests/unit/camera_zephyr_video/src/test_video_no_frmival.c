/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake video controller backing the alp,test-video-no-frmival DT node
 * (see ../dts/bindings/alp,test-video-no-frmival.yaml and
 * ../boards/native_sim.overlay).  Gives
 * src/backends/camera/zephyr_video.c's z_open() a real device to reach
 * the fps step with -- get_caps / set_format / get_format / enqueue /
 * dequeue are all implemented, just enough for open()/close() to run
 * end to end -- but .set_frmival / .get_frmival / .enum_frmival are
 * deliberately absent, so camera_apply_fps() (camera_frmival.h) sees
 * video_set_frmival() return -ENOSYS: "this device has no frame-rate
 * control at all" (#2278).
 */

#define DT_DRV_COMPAT alp_test_video_no_frmival

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/video.h>

/* Fixed 64x64 RGB565 -- inside video_bits_per_pixel()'s known table, and
 * big enough for zephyr_video.c's z_open() to size a real (non-dummy)
 * buffer allocation. */
#define TEST_VIDEO_WIDTH  64
#define TEST_VIDEO_HEIGHT 64

static int test_video_get_caps(const struct device *dev, struct video_caps *caps)
{
	ARG_UNUSED(dev);
	/* No format_caps list -- z_open() takes its "no portable FourCC
	 * negotiated" fallback branch (video_get_format() below) regardless
	 * of the caller's requested format/width/height. */
	caps->format_caps    = NULL;
	caps->min_vbuf_count = 1;
	return 0;
}

static int test_video_set_format(const struct device *dev, struct video_format *fmt)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(fmt);
	return 0;
}

static int test_video_get_format(const struct device *dev, struct video_format *fmt)
{
	ARG_UNUSED(dev);
	fmt->pixelformat = VIDEO_PIX_FMT_RGB565;
	fmt->width       = TEST_VIDEO_WIDTH;
	fmt->height      = TEST_VIDEO_HEIGHT;
	fmt->pitch       = 0u;
	return 0;
}

static int test_video_enqueue(const struct device *dev, struct video_buffer *vbuf)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(vbuf);
	/* Accept unconditionally -- this test only exercises open()/close(),
	 * never start()/capture(), so nothing needs to actually queue. */
	return 0;
}

static int
test_video_dequeue(const struct device *dev, struct video_buffer **vbuf, k_timeout_t timeout)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(vbuf);
	ARG_UNUSED(timeout);
	/* Nothing was ever queued for real -- close()'s drain loop
	 * (_release_vbufs) stops on the first nonzero return. */
	return -EAGAIN;
}

static DEVICE_API(video, test_video_no_frmival_api) = {
	.get_caps   = test_video_get_caps,
	.set_format = test_video_set_format,
	.get_format = test_video_get_format,
	.enqueue    = test_video_enqueue,
	.dequeue    = test_video_dequeue,
	/* Deliberately no .set_frmival / .get_frmival / .enum_frmival --
	 * the whole point of this fake (#2278). */
};

static int test_video_no_frmival_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

#define TEST_VIDEO_NO_FRMIVAL_INIT(n) \
	DEVICE_DT_INST_DEFINE(n, \
	                      test_video_no_frmival_init, \
	                      NULL, \
	                      NULL, \
	                      NULL, \
	                      POST_KERNEL, \
	                      CONFIG_VIDEO_INIT_PRIORITY, \
	                      &test_video_no_frmival_api);

DT_INST_FOREACH_STATUS_OKAY(TEST_VIDEO_NO_FRMIVAL_INIT)
