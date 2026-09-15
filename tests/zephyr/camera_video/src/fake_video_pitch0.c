/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Fake drivers/video/ device for the alp-sdk camera_video ztest (#1628,
 * Part 2).  Advertises VIDEO_PIX_FMT_RGB24 (24 bpp -- sizeable via
 * Zephyr's video_bits_per_pixel table) but deliberately never fills
 * video_format.pitch in set_format, the exact arrangement in which a
 * driver-reported pitch cannot be trusted and the caller must derive
 * bytes-per-pixel from the negotiated fourcc instead.  enqueue() refuses
 * any video_buffer smaller than width * height * 3 bytes, standing in for
 * the CSI-2/ISP DMA overrun an under-sized buffer would suffer on real
 * silicon.
 */

#define DT_DRV_COMPAT alp_fake_video_pitch0

#include <errno.h>

#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>

static const struct video_format_cap _fmts[] = {
	{
	    .pixelformat = VIDEO_PIX_FMT_RGB24,
	    .width_min   = 8,
	    .width_max   = 640,
	    .height_min  = 8,
	    .height_max  = 480,
	    .width_step  = 1,
	    .height_step = 1,
	},
	{ 0 },
};

struct fake_video_pitch0_data {
	struct video_format fmt;
};

static int _get_caps(const struct device *dev, struct video_caps *caps)
{
	ARG_UNUSED(dev);
	caps->format_caps    = _fmts;
	caps->min_vbuf_count = 1;
	return 0;
}

static int _set_fmt(const struct device *dev, struct video_format *fmt)
{
	struct fake_video_pitch0_data *data = dev->data;

	if (fmt->pixelformat != VIDEO_PIX_FMT_RGB24) {
		return -ENOTSUP;
	}
	/* Deliberately leave fmt->pitch untouched (0) -- the arrangement
	 * under test. */
	data->fmt = *fmt;
	return 0;
}

static int _get_fmt(const struct device *dev, struct video_format *fmt)
{
	struct fake_video_pitch0_data *data = dev->data;

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
	struct fake_video_pitch0_data *data = dev->data;
	uint32_t                       need = (uint32_t)data->fmt.width * data->fmt.height * 3u;

	if (buf->size < need) {
		return -ENOBUFS;
	}
	return 0;
}

static const struct video_driver_api _api = {
	.set_format = _set_fmt,
	.get_format = _get_fmt,
	.set_stream = _set_stream,
	.get_caps   = _get_caps,
	.enqueue    = _enqueue,
};

static struct fake_video_pitch0_data _data0;

DEVICE_DT_INST_DEFINE(0, NULL, NULL, &_data0, NULL, POST_KERNEL, CONFIG_VIDEO_INIT_PRIORITY, &_api);
