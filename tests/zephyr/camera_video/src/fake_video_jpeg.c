/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake drivers/video/ device for the alp-sdk camera_video ztest (#1628
 * review follow-up).  Boots already set to VIDEO_PIX_FMT_JPEG with pitch
 * left at 0 -- video_bits_per_pixel() has no table entry for JPEG and
 * returns 0, and this is the exact shape a real MJPEG sensor (e.g.
 * drivers/video/ov2640.c, which advertises VIDEO_PIX_FMT_JPEG at ten
 * resolutions and never writes fmt->pitch) exposes before any
 * alp_camera_open() negotiation.
 *
 * get_caps() advertises no format_caps entries whose pixelformat the
 * portable ALP_PIXFMT_* enum can request, so a caller that asks for the
 * enum's zero value (ALP_PIXFMT_MONO_VLSB, which _to_video_fourcc() maps
 * to 0) takes the else/readback branch in zephyr_video.c / v2n_n44_isp.c:
 * video_get_format() reads back this driver's already-JPEG default
 * instead of negotiating a new one.  That is the path the #1628 review
 * found hard-refusing with ALP_ERR_NOSUPPORT instead of falling back to
 * the flat w*h*2 heuristic bound compressed formats need.
 */

#define DT_DRV_COMPAT alp_fake_video_jpeg0

#include <errno.h>

#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>

#define FAKE_JPEG_WIDTH  320
#define FAKE_JPEG_HEIGHT 240

struct fake_video_jpeg_data {
	struct video_format fmt;
};

static int _get_caps(const struct device *dev, struct video_caps *caps)
{
	ARG_UNUSED(dev);
	caps->format_caps    = NULL;
	caps->min_vbuf_count = 1;
	return 0;
}

static int _get_fmt(const struct device *dev, struct video_format *fmt)
{
	struct fake_video_jpeg_data *data = dev->data;

	*fmt = data->fmt;
	return 0;
}

static int _set_stream(const struct device *dev, bool enable, enum video_buf_type type)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(enable);
	ARG_UNUSED(type);
	return 0;
}

static int _enqueue(const struct device *dev, struct video_buffer *buf)
{
	struct fake_video_jpeg_data *data = dev->data;
	/* Stand-in for the flat w*h*2 heuristic bound the backend must fall
	 * back to for JPEG -- refuses anything smaller, standing in for the
	 * DMA overrun an under-sized buffer would suffer on real silicon. */
	uint32_t need = (uint32_t)data->fmt.width * data->fmt.height * 2u;

	if (buf->size < need) {
		return -ENOBUFS;
	}
	return 0;
}

static const struct video_driver_api _api = {
	.get_format = _get_fmt,
	.set_stream = _set_stream,
	.get_caps   = _get_caps,
	.enqueue    = _enqueue,
};

static struct fake_video_jpeg_data _data0 = {
	.fmt =
	    {
	        .pixelformat = VIDEO_PIX_FMT_JPEG,
	        .width       = FAKE_JPEG_WIDTH,
	        .height      = FAKE_JPEG_HEIGHT,
	        .pitch       = 0,
	    },
};

DEVICE_DT_INST_DEFINE(0, NULL, NULL, &_data0, NULL, POST_KERNEL, CONFIG_VIDEO_INIT_PRIORITY, &_api);
