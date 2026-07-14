/*
 * Copyright (C) 2026 Alif Semiconductor.
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ====== ADR 0017 Tier-2 (vendored fork-driver copy, INTERIM) ======
 * The Alif Ensemble ISP-Pico image-signal-processor is driven by a vendored
 * copy of the Apache-2.0 zephyr_alif fork driver (drivers/video/isp_pico.c,
 * compatible "vsi,isp-pico").  It is a true m2m video device (it has BOTH an
 * input EP fed by the camera/CSI controller and an output EP that DMAs the
 * processed frame to memory) and it links the hal_alif libisp wrapper (the
 * Vivante ISP middleware, a proprietary BLOB) -- opt-in only, via
 * USE_ALIF_ISP_LIB / CONFIG_VIDEO_ISP_VSI.  Upstream Zephyr v4.4 ships no
 * ISP-Pico class driver, so this is a genuine fork-driver copy carried in-tree
 * so it survives a `west update`.  Retire onto the opt-in sdk-alif fork
 * compatible once the ISP node is repointed AND bench-verified.
 * See docs/adr/0017-alp-sdk-over-the-vendor-sdk.md.
 * ==========================================================================
 *
 * Vendored from the fork, then PORTED to the upstream Zephyr v4.4 video API by
 * Alp Lab AB.  The fork driver targeted the OLDER video API -- its
 * video_driver_api callbacks took `enum video_endpoint_id ep`
 * (set_format/get_format/get_caps/flush/enqueue/dequeue/set_signal) and a
 * value-pointer ctrl API, both of which upstream v4.4 REMOVED.  The v4.4 deltas
 * applied here (each marked "v4.4 video-API shim (Alp Lab AB)" at the call
 * site):
 *   - the m2m `enum video_endpoint_id ep` dispatch is re-expressed against
 *     v4.4's per-buffer-type model: set_format/get_format switch on
 *     `fmt->type` (VIDEO_EP_IN -> VIDEO_BUF_TYPE_INPUT,
 *     VIDEO_EP_OUT -> VIDEO_BUF_TYPE_OUTPUT); there is no v4.4 equivalent of
 *     VIDEO_EP_ALL (the framework calls set_format once per type), so that
 *     case is folded away;
 *   - get_caps switches on `caps->type` instead of `ep`;
 *   - set_stream(dev, bool) gained an `enum video_buf_type type` param;
 *     video_stream_start/_stop on the upstream controller now take
 *     VIDEO_BUF_TYPE_OUTPUT (the controller is the capture source);
 *   - the forwarding helpers lose their `ep` arg
 *     (video_set_format/video_get_format/video_get_caps/video_flush); when
 *     forwarding to the controller (a capture device) the fmt/caps `.type` is
 *     forced to VIDEO_BUF_TYPE_OUTPUT so the controller fills/accepts its
 *     OUTPUT side;
 *   - the value-pointer .set_ctrl/.get_ctrl callbacks are dropped (see the
 *     note above the DEVICE_API table); this also removes the only callers of
 *     isp_vsi_set_param/isp_vsi_get_param -- which is intentional (see the
 *     HAL_ALIF VERSION MISMATCH note below: those symbols do not exist in the
 *     locally vendored wrapper).
 *
 * !!! HAL_ALIF VERSION MISMATCH (FLAGGED) !!!
 * This 2026 isp_pico.c was authored against a NEWER hal_alif libisp wrapper than
 * the one vendored locally (modules/hal/alif/drivers/isp/isp_wrapper, 2025).
 * The local wrapper (isp_api_wrapper.c) exports
 * isp_vsi_init/update_cfg/uninit/bottom_half/start/stop/enqueue/dequeue but DOES
 * NOT export isp_vsi_set_param / isp_vsi_get_param (called by the fork's ctrl
 * path -- now dropped in the v4.4 port, so no longer referenced) and its
 * isp_vsi_bottom_half() is 2-arg `(init_cfg, mi_mis)` whereas this driver calls
 * the 3-arg `(dev, init_cfg, mi_mis)`.  It also #includes
 * <zephyr/drivers/video/isp-vsi.h>, which the local wrapper does NOT ship (it
 * ships isp_conf.h / isp_param_conf.h instead).  Consequently
 * CONFIG_VIDEO_ISP_VSI is opt-in default n: the driver compiles-but-WILL-NOT-
 * LINK against the local hal_alif until the wrapper is bumped to the matching
 * version.  Do NOT fabricate the missing API.
 * vendor-ext, BENCH-UNVERIFIED, ISP=Vivante blob (opt-in).
 */
#define DT_DRV_COMPAT vsi_isp_pico

#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ISP, CONFIG_VIDEO_LOG_LEVEL);

#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/drivers/pinctrl.h>

#include "isp_pico.h"
#include <zephyr/drivers/video/video_alif.h>
#include <soc_memory_map.h>
#include <zephyr/cache.h>

#define WORKQ_STACK_SIZE 4096
#define WORKQ_PRIORITY   7
K_KERNEL_STACK_DEFINE(isp_cb_workq, WORKQ_STACK_SIZE);

#define ISP_VIDEO_FORMAT_CAP(format, width, height)                                             \
	{                                                                                       \
		.pixelformat = (format), .width_min = (0), .width_max = (width),                \
		.height_min = (0), .height_max = (height), .width_step = 8, .height_step = 4,   \
	}

#define ISP_VIDEO_FIXED_FORMAT_CAP(format, width, height)                                          \
	{                                                                                          \
		.pixelformat = (format), .width_min = (width), .width_max = (width),               \
		.height_min = (height), .height_max = (height), .width_step = 0, .height_step = 0, \
	}

static const struct video_format_cap supported_input_fmts[] = {
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_BGGR8, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_GBRG8, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_GRBG8, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_RGGB8, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_BGGR10, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_GBRG10, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_GRBG10, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_RGGB10, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_BGGR12, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_GBRG12, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_GRBG12, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_RGGB12, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_GREY, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_Y10P, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_YUYV, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_YVYU, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_UYVY, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_VYUY, 1920, 1080),
	{ 0 },
};

static const struct video_format_cap supported_tpg_fmts[] = {
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_BGGR8, 1280, 720),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_GBRG8, 1280, 720),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_GRBG8, 1280, 720),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_RGGB8, 1280, 720),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_BGGR10, 1280, 720),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_GBRG10, 1280, 720),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_GRBG10, 1280, 720),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_RGGB10, 1280, 720),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_BGGR12, 1280, 720),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_GBRG12, 1280, 720),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_GRBG12, 1280, 720),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_RGGB12, 1280, 720),

	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_BGGR8, 1920, 1080),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_GBRG8, 1920, 1080),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_GRBG8, 1920, 1080),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_RGGB8, 1920, 1080),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_BGGR10, 1920, 1080),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_GBRG10, 1920, 1080),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_GRBG10, 1920, 1080),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_RGGB10, 1920, 1080),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_BGGR12, 1920, 1080),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_GBRG12, 1920, 1080),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_GRBG12, 1920, 1080),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_RGGB12, 1920, 1080),

	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_BGGR8, 3840, 2160),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_GBRG8, 3840, 2160),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_GRBG8, 3840, 2160),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_RGGB8, 3840, 2160),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_BGGR10, 3840, 2160),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_GBRG10, 3840, 2160),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_GRBG10, 3840, 2160),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_RGGB10, 3840, 2160),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_BGGR12, 3840, 2160),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_GBRG12, 3840, 2160),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_GRBG12, 3840, 2160),
	ISP_VIDEO_FIXED_FORMAT_CAP(VIDEO_PIX_FMT_RGGB12, 3840, 2160),
	{ 0 },
};

static const struct video_format_cap supported_output_fmts[] = {
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_BGGR8, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_GBRG8, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_GRBG8, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_RGGB8, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_BGGR10, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_GBRG10, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_GRBG10, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_RGGB10, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_BGGR12, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_GBRG12, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_GRBG12, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_RGGB12, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_NV12, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_NV16, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_YUV422P, 19200, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_YUV420, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_YUYV, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_GREY, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_Y10, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_Y12, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_RGB888_PLANAR_PRIVATE, 1920, 1080),
	{ 0 },
};

static int get_format_cap(uint32_t fourcc_fmt,
		const struct video_format_cap supported_fmts[])
{
	for (int i = 0; supported_fmts[i].pixelformat; i++) {
		if (fourcc_fmt == supported_fmts[i].pixelformat) {
			return i;
		}
	}

	return -1;
}

static int find_format(struct video_format *fmt,
		const struct video_format_cap supported_fmts[])
{
	for (int i = 0; supported_fmts[i].pixelformat; i++) {
		if (fmt->pixelformat == supported_fmts[i].pixelformat &&
		    fmt->width >= supported_fmts[i].width_min &&
		    fmt->width <= supported_fmts[i].width_max &&
		    fmt->height >= supported_fmts[i].height_min &&
		    fmt->height <= supported_fmts[i].height_max) {
			/* The matching format supported by ISP is found. */
			return 0;
		}
	}

	return -ENOTSUP;
}

static int isp_attach_buffer_to_hw(const struct device *dev, struct video_buffer *vbuf)
{
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	struct isp_data *data = dev->data;
	uint32_t planes[3] = {};
	size_t size_plane;
	int num_planes;
	int i;

	struct channel_parameters *channel = &data->init_cfg.channel;

	num_planes = fourcc_to_numplanes(channel->output_fmt.pixelformat);
	if (num_planes == 0) {
		LOG_ERR("Unsupported format!");
		return -ENOTSUP;
	}

	for (i = 0; i < num_planes; i++) {
		if (!i) {
			planes[i] = POINTER_TO_UINT(local_to_global(vbuf->buffer));
		} else {
			size_plane = fourcc_to_plane_size(channel->output_fmt.pixelformat,
					i - 1, vbuf->size);
			if (size_plane == 0 || size_plane > vbuf->size) {
				LOG_ERR("Unsupported format!");
				return -ENOTSUP;
			}

			planes[i] = (planes[i-1] + size_plane);
		}
	}

	LOG_DBG("planes: 0x%08x 0x%08x 0x%08x", planes[0], planes[1], planes[2]);
	sys_write32(planes[0], regs + ISP_MI_MP_Y_BASE_AD_INIT);
	sys_write32(planes[1], regs + ISP_MI_MP_CB_BASE_AD_INIT);
	sys_write32(planes[2], regs + ISP_MI_MP_CR_BASE_AD_INIT);

	sys_set_bits(regs + ISP_MI_INIT, MI_INIT_CFG_UPD);

	return 0;
}

static void hw_disable_mi_interrupts(uintptr_t regs, uint32_t mask)
{
	sys_clear_bits(regs + ISP_MI_IMSC, mask);
}

static void isp_bottom_half(const struct device *dev)
{
	enum video_signal_result signal_status = VIDEO_BUF_DONE;
	const struct isp_config *config = dev->config;
	struct isp_data *data = dev->data;
	struct video_buffer *vbuf = NULL;

	int ret;

	/* Do bottom half processing of all the modules at the end of frame. */
	isp_vsi_bottom_half(dev, &data->init_cfg, data->mi_mis);


	vbuf = k_fifo_peek_head(&data->fifo_in);
	if (vbuf == NULL) {
		LOG_ERR("Unexpected condition! Empty IN-FIFO");
		data->is_streaming = false;
		signal_status = VIDEO_BUF_ERROR;
		goto isp_bottom_done;
	}

	if (data->curr_vid_buf != (uint32_t)vbuf->buffer) {
		signal_status = VIDEO_BUF_ERROR;
		data->is_streaming = false;
		LOG_ERR("Unknown Video Buffer assigned to ISP.");
		goto isp_bottom_done;
	}

	vbuf = k_fifo_get(&data->fifo_in, K_NO_WAIT);
	if (!vbuf) {
		LOG_ERR("Failed to get video buffer from IN-FIFO, "
			"despite IN-FIFO having data");
		data->is_streaming = false;
		signal_status = VIDEO_BUF_ERROR;
		goto isp_bottom_done;
	}

	vbuf->timestamp = k_uptime_get_32();

	k_fifo_put(&data->fifo_out, vbuf);

	vbuf = k_fifo_peek_head(&data->fifo_in);
	if (vbuf == NULL) {
		LOG_DBG("No more empty buffers in the IN-FIFO. "
			"Stopping video capture. If re-queued, restart stream.");
		data->is_streaming = false;
		signal_status = VIDEO_BUF_DONE;
		goto isp_bottom_done;
	}
	data->curr_vid_buf = (uint32_t) vbuf->buffer;

	ret = isp_attach_buffer_to_hw(dev, vbuf);
	if (ret) {
		LOG_ERR("Failed to attach buffer to hardware!");
		data->is_streaming = false;
		signal_status = VIDEO_BUF_DONE;
		goto isp_bottom_done;
	}

isp_bottom_done:
	if (!data->is_streaming) {
		/* v4.4 video-API shim (Alp Lab AB): video_stream_stop gained an
		 * `enum video_buf_type`; the controller is the capture source ->
		 * VIDEO_BUF_TYPE_OUTPUT.
		 */
		video_stream_stop(config->controller, VIDEO_BUF_TYPE_OUTPUT);
		data->curr_vid_buf = 0;
	}

	LOG_DBG("current video buffer - 0x%08x", data->curr_vid_buf);
#if defined(CONFIG_POLL)
	if (data->signal) {
		k_poll_signal_raise(data->signal, signal_status);
	}
#endif /* defined(CONFIG_POLL) */
}

static void isp_cb_work(struct k_work *work)
{
	struct isp_data *data = CONTAINER_OF(work, struct isp_data, cb_work);

	/* Call a helper to process the things further. */
	isp_bottom_half(data->dev);
}

static void isp_isr_handler(const struct device *dev)
{
	struct isp_data *data = dev->data;

	uint32_t isp_intr_err_mask = INTR_SIZE_ERR | INTR_DATALOSS;
	static bool is_not_corrupted_frame = true;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	uint32_t mi_int_st;
	uint32_t int_st;

	int_st = sys_read32(regs + ISP_MIS);
	sys_write32(int_st, regs + ISP_ICR);

	mi_int_st = sys_read32(regs + ISP_MI_MIS);
	sys_write32(mi_int_st, regs + ISP_MI_ICR);

	data->mi_mis = mi_int_st;

	if (int_st & INTR_EXP_END) {
		LOG_DBG("Exposure measurement complete.");
	}

	if (int_st & INTR_H_START) {
		LOG_DBG("H-Sync detected");
	}

	if (int_st & INTR_V_START) {
		LOG_DBG("V-Sync detected");
	}

	if (int_st & INTR_FRAME_IN) {
		LOG_DBG("Sampled Input frame is complete.");
	}

	if (int_st & INTR_AWB_DONE) {
		LOG_DBG("White balancing measurement complete");
	}

	if (int_st & INTR_SIZE_ERR) {
		LOG_ERR("Picture size violation occurred; incorrect programming");
	}

	if (int_st & INTR_DATALOSS) {
		LOG_ERR("Loss of data within a line; processing failure");
	}

	if (int_st & isp_intr_err_mask) {
		LOG_ERR("Frame capture error. int_st - 0x%08x", int_st);
		is_not_corrupted_frame = false;
#if defined(CONFIG_POLL)
		if (data->signal) {
			k_poll_signal_raise(data->signal, VIDEO_BUF_ERROR);
		}
#endif /* defined(CONFIG_POLL) */
	}

	if (mi_int_st & MI_INTR_WRAP_MP_CR) {
		LOG_DBG("Main picture Cr address wrap");
	}

	if (mi_int_st & MI_INTR_WRAP_MP_CB) {
		LOG_DBG("Main picture Cb address wrap");
	}

	if (mi_int_st & MI_INTR_WRAP_MP_Y) {
		LOG_DBG("Main picture Y address wrap");
	}

	if (mi_int_st & MI_INTR_FILL_MP_Y) {
		LOG_DBG("Main picture fill level interrupt");
	}

	if (mi_int_st & MI_INTR_MBLK_LINE) {
		LOG_DBG("Main picture Macro block line interrupt");
	}

	if (mi_int_st & MI_INTR_MP_FRAME_END) {
		LOG_DBG("End of Frame at MI interface of Main picture.");
		if (is_not_corrupted_frame) {
			k_work_submit_to_queue(&data->cb_workq, &data->cb_work);
		} else {
			is_not_corrupted_frame = true;
		}
	}
}

/* v4.4 video-API shim (Alp Lab AB): dropped the `enum video_endpoint_id ep`
 * param; the m2m dispatch now switches on `fmt->type` (VIDEO_BUF_TYPE_INPUT for
 * the camera-fed input EP, VIDEO_BUF_TYPE_OUTPUT for the DMA output EP).  The
 * old VIDEO_EP_ALL case has no v4.4 equivalent -- the framework now calls
 * set_format once per type -- so it is folded away; the INPUT/OUTPUT branch
 * bodies are otherwise verbatim.
 */
int isp_set_fmt(const struct device *dev,
		struct video_format *fmt)
{
	const struct isp_config *config = dev->config;
	struct isp_data *data = dev->data;

	struct channel_parameters *channel = &data->init_cfg.channel;
	struct port_parameters *port = &data->init_cfg.port;
	int ret = -ENODEV;

	if (!fmt) {
		LOG_ERR("Illegal format to set!");
		return -EINVAL;
	}

	switch (fmt->type) {
	case VIDEO_BUF_TYPE_INPUT:
		if (!memcmp(fmt, &port->port_fmt, sizeof(*fmt))) {
			/* Nothing to do */
			return 0;
		}

		ret = find_format(fmt, supported_input_fmts);
		if (ret) {
			LOG_ERR("Desired format is not supported by the ISP Input EP!");
			return ret;
		}

		/* v4.4 video-API shim (Alp Lab AB): video_set_format lost its `ep`
		 * arg; force the forwarded fmt->type to OUTPUT so the controller
		 * (a capture device) accepts it from its OUTPUT POV.
		 */
		fmt->type = VIDEO_BUF_TYPE_OUTPUT;
		ret = video_set_format(config->controller, fmt);
		fmt->type = VIDEO_BUF_TYPE_INPUT;
		if (ret) {
			LOG_ERR("Failed to set desired format on camera pipeline!");
			return ret;
		}

		/* Cache the desired input format. */
		port->port_fmt = *fmt;
		break;
	case VIDEO_BUF_TYPE_OUTPUT:
		if (!memcmp(fmt, &channel->output_fmt, sizeof(*fmt))) {
			/* Nothing to do */
			return 0;
		}

		ret = find_format(fmt, supported_output_fmts);
		if (ret) {
			LOG_ERR("Desired format is not supported by the ISP Output EP!");
			return ret;
		}

		channel->output_fmt = *fmt;
		break;
	default:
		LOG_ERR("Unsupported buffer type!");
		return -EINVAL;

	}

	return 0;
}

/* v4.4 video-API shim (Alp Lab AB): dropped the `enum video_endpoint_id ep`
 * param; the m2m dispatch now switches on `fmt->type` (VIDEO_BUF_TYPE_INPUT /
 * VIDEO_BUF_TYPE_OUTPUT).  Unknown buffer types return -ENOTSUP.
 */
int isp_get_fmt(const struct device *dev,
		struct video_format *fmt)
{
	const struct isp_config *config = dev->config;
	struct isp_data *data = dev->data;

	struct channel_parameters *channel = &data->init_cfg.channel;
	struct port_parameters *port = &data->init_cfg.port;
	int ret;

	if (!fmt) {
		return -EINVAL;
	}

	switch (fmt->type) {
	case VIDEO_BUF_TYPE_INPUT:
		if (!port->port_fmt.pixelformat) {
			/* v4.4 video-API shim (Alp Lab AB): video_get_format lost its
			 * `ep` arg; force the forwarded fmt->type to OUTPUT so the
			 * controller (a capture device) fills its OUTPUT format.
			 */
			fmt->type = VIDEO_BUF_TYPE_OUTPUT;
			ret = video_get_format(config->controller, fmt);
			fmt->type = VIDEO_BUF_TYPE_INPUT;
			if (ret) {
				return ret;
			}

			ret = find_format(fmt, supported_input_fmts);
			if (ret) {
				LOG_ERR("Pipeline running on unsupported format by ISP!");
				return ret;
			}

			port->port_fmt = *fmt;
		}

		*fmt = port->port_fmt;
		break;
	case VIDEO_BUF_TYPE_OUTPUT:
		if (!channel->output_fmt.pixelformat) {
			uint32_t tmp_fmt = VIDEO_PIX_FMT_RGB888_PLANAR_PRIVATE;
			int i;

			i = get_format_cap(tmp_fmt, supported_output_fmts);
			if (i == -1) {
				LOG_ERR("Failed to set output format for ISP!");
				return -EINVAL;
			}

			/*
			 * If input format is also not set, use
			 * RGB888 planar output format.
			 */
			channel->output_fmt.pixelformat =
				supported_output_fmts[i].pixelformat;
			channel->output_fmt.height =
				supported_output_fmts[i].height_max;
			channel->output_fmt.width =
				supported_output_fmts[i].width_max;
			channel->output_fmt.pitch =
				(video_bits_per_pixel(tmp_fmt) *
				 channel->output_fmt.width) >> 3;
		}

		*fmt = channel->output_fmt;
		break;
	default:
		LOG_ERR("Unsupported buffer type!");
		return -ENOTSUP;
	}
	return 0;
}

static int isp_stream_start(const struct device *dev)
{
	const struct isp_config *config = dev->config;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	struct isp_data *data = dev->data;
	struct video_buffer *vbuf;
	struct video_buffer vbuf2;

	struct port_parameters *port = &data->init_cfg.port;
	uint32_t tmp;
	int ret;

	/* Cancel any stale work from previous session before starting */
	struct k_work_sync sync;

	k_work_cancel_sync(&data->cb_work, &sync);

	if (data->is_streaming) {
		LOG_DBG("Already streaming");
		return -EBUSY;
	}

	vbuf = k_fifo_peek_head(&data->fifo_in);
	if (vbuf == NULL) {
		LOG_ERR("Unexpected condition! Empty IN-FIFO. Can't start streaming!");
		data->is_streaming = false;
		return -ENOBUFS;
	}

	data->curr_vid_buf = POINTER_TO_UINT(vbuf->buffer);

	/* Update ISP configuration to the middleware */
	switch (port->port_fmt.pixelformat) {
	case VIDEO_PIX_FMT_YUYV:
		port->seq = YCBYCR;
		break;
	case VIDEO_PIX_FMT_YVYU:
		port->seq = YCRYCB;
		break;
	case VIDEO_PIX_FMT_VYUY:
		port->seq = CRYCBY;
		break;
	case VIDEO_PIX_FMT_UYVY:
		port->seq = CBYCRY;
		break;
	}

	port->sns_rect.width = port->port_fmt.width;
	port->sns_rect.height = port->port_fmt.height;

	port->in_form_rect.width = port->port_fmt.width;
	port->in_form_rect.height = port->port_fmt.height;

	port->image_stabilization_rect.top = port->in_form_rect.top;
	port->image_stabilization_rect.left = port->in_form_rect.left;
	port->image_stabilization_rect.width = port->in_form_rect.width;
	port->image_stabilization_rect.height = port->in_form_rect.height;

	port->out_form_rect.width = port->port_fmt.width - (port->out_form_rect.left << 1);
	port->out_form_rect.height = port->port_fmt.height - (port->out_form_rect.top << 1);

	ret = isp_vsi_update_cfg(&data->init_cfg);
	if (ret) {
		LOG_ERR("Failed to update ISP config to input/output formats and ROI!");
		data->curr_vid_buf = 0;
		return ret;
	}

	tmp = sys_read32(regs + ISP_ACQ_PROP);
	tmp &= ~(ACQ_PROP_PIN_MAPPING_MASK << ACQ_PROP_PIN_MAPPING_SHIFT);

	switch (pix_fmt_bpp(port->port_fmt.pixelformat)) {
	case 10:
		tmp |= (1 << ACQ_PROP_PIN_MAPPING_SHIFT);
		break;
	case 8:
		tmp |= (2 << ACQ_PROP_PIN_MAPPING_SHIFT);
		break;
	case 12:
	default:
		tmp |= (0 << ACQ_PROP_PIN_MAPPING_SHIFT);
		break;
	}
	sys_write32(tmp, regs + ISP_ACQ_PROP);

	ret = isp_vsi_enqueue(&data->init_cfg, vbuf);
	if (ret) {
		LOG_ERR("Failed to assign buffer to hardware!");
		data->curr_vid_buf = 0;
		return ret;
	}

	/* Set is_streaming BEFORE starting hardware to prevent
	 * bottom_half from stopping CPI mid-start
	 */
	data->is_streaming = true;

	ret = isp_vsi_start(&data->init_cfg);
	if (ret) {
		LOG_ERR("Failed to start stream!");
		data->is_streaming = false;
		goto dequeue_buf;
	}

	/* v4.4 video-API shim (Alp Lab AB): video_stream_start gained an
	 * `enum video_buf_type`; the controller is the capture source ->
	 * VIDEO_BUF_TYPE_OUTPUT.
	 */
	ret = video_stream_start(config->controller, VIDEO_BUF_TYPE_OUTPUT);
	if (ret) {
		LOG_ERR("Failed to start stream for Endpoint device: %s!",
				config->controller->name);
		data->is_streaming = false;
		goto stop_isp_stream;
	}

	return 0;

stop_isp_stream:
	ret = isp_vsi_stop(&data->init_cfg);
	if (ret) {
		LOG_ERR("Failed to stop ISP device streaming");
		return ret;
	}
dequeue_buf:
	ret = isp_vsi_dequeue(&data->init_cfg, &vbuf2);
	if (ret) {
		LOG_ERR("Failed to dequeue buffer back!");
		return ret;
	}

	return 0;
}

static int isp_stream_stop(const struct device *dev)
{
	const struct isp_config *config = dev->config;
	struct isp_data *data = dev->data;
	int ret;

	if (!data->is_streaming) {
		LOG_DBG("Already stopped streaming!");
		return 0;
	}

	/* v4.4 video-API shim (Alp Lab AB): video_stream_stop gained an
	 * `enum video_buf_type`; the controller is the capture source ->
	 * VIDEO_BUF_TYPE_OUTPUT.
	 */
	ret = video_stream_stop(config->controller, VIDEO_BUF_TYPE_OUTPUT);
	if (ret) {
		LOG_ERR("Failed to stop streaming in pipeline!");
		return ret;
	}

	ret = isp_vsi_stop(&data->init_cfg);
	if (ret) {
		LOG_ERR("Failed to stop ISP from streaming!");
		return ret;
	}

	data->curr_vid_buf = 0;
	data->is_streaming = false;

	return 0;
}

/* v4.4 video-API shim (Alp Lab AB): set_stream gained an `enum video_buf_type
 * type` param (unused here -- this m2m device streams a single pipeline).
 */
static int isp_set_stream(const struct device *dev, bool enable, enum video_buf_type type)
{
	ARG_UNUSED(type);

	if (enable) {
		return isp_stream_start(dev);
	} else {
		return isp_stream_stop(dev);
	}
}

/* v4.4 video-API shim (Alp Lab AB): dropped the `enum video_endpoint_id ep`
 * param; the m2m dispatch now switches on the caller-set `caps->type`
 * (VIDEO_BUF_TYPE_INPUT / VIDEO_BUF_TYPE_OUTPUT).
 */
static int isp_get_caps(const struct device *dev,
		struct video_caps *caps)
{
	const struct isp_config *config = dev->config;
	int err = -ENODEV;

	if (caps->type == VIDEO_BUF_TYPE_OUTPUT) {
		caps->format_caps = supported_output_fmts;
	} else if (caps->type == VIDEO_BUF_TYPE_INPUT) {
		if (config->controller) {
			/*
			 * Camera controlled output EP should have same fmt as
			 * ISP input EP.
			 *
			 * v4.4 video-API shim (Alp Lab AB): video_get_caps lost
			 * its `ep` arg; force caps->type to OUTPUT so the
			 * controller fills its output caps, then restore INPUT.
			 */
			caps->type = VIDEO_BUF_TYPE_OUTPUT;
			err = video_get_caps(config->controller, caps);
			caps->type = VIDEO_BUF_TYPE_INPUT;
			if (err) {
				LOG_ERR("Failed to get caps from camera-controller!");
				return err;
			}
		} else if (config->tpg_img_idx != IMG_DISABLED) {
			/* When TPG is enabled! */
			caps->format_caps = supported_tpg_fmts;
		} else {
			/* Neither TPG nor Camera controller is enabled. */
			return -EINVAL;
		}
	} else {
		return -ENOTSUP;
	}

	caps->min_vbuf_count = ISP_MIN_VBUF;

	return 0;
}

/* v4.4 video-API shim (Alp Lab AB): dropped the `enum video_endpoint_id ep`
 * param; the forwarded video_flush() also loses its `ep` arg.
 */
static int isp_flush(const struct device *dev, bool cancel)
{
	const struct isp_config *config = dev->config;
	struct isp_data *data = dev->data;

	uintptr_t regs = DEVICE_MMIO_GET(dev);
	struct video_buffer *vbuf = NULL;

	int ret;

	if (cancel) {
		/* Case when video stream processing needs to be stopped. */
		hw_disable_mi_interrupts(regs, MI_INTR_MP_FRAME_END);

		for (int i = 0; (i < 20) &&
				(sys_read32(regs + ISP_MI_RIS) & MI_INTR_MP_FRAME_END); i++) {
			k_msleep(10);
		}

		if (sys_read32(regs + ISP_MI_RIS) & MI_INTR_MP_FRAME_END) {
			LOG_ERR("Failed to observe frame end!");
			return -EBUSY;
		}

		ret = isp_vsi_stop(&data->init_cfg);
		if (ret) {
			LOG_ERR("Failed to stop ISP device!");
			return ret;
		}

		while ((vbuf = k_fifo_get(&data->fifo_in, K_NO_WAIT))) {
			k_fifo_put(&data->fifo_out, vbuf);
			LOG_DBG("Video Buffer Aborted!!! - 0x%x", (uint32_t)vbuf->buffer);
#if defined(CONFIG_POLL)
			if (data->signal) {
				k_poll_signal_raise(data->signal, VIDEO_BUF_ABORTED);
			}
#endif /* defined(CONFIG_POLL) */
		}
	} else {
		/* Case when video stream processing need not be stopped. */
		if (!data->curr_vid_buf) {
			while ((vbuf = k_fifo_get(&data->fifo_in, K_NO_WAIT))) {
				k_fifo_put(&data->fifo_out, vbuf);
			}
		}

		while (!k_fifo_is_empty(&data->fifo_in)) {
			k_msleep(1);
		}
	}

	data->curr_vid_buf = 0;
	data->is_streaming = false;

	video_flush(config->controller, cancel);

	return 0;
}

/* v4.4 video-API shim (Alp Lab AB): dropped the `enum video_endpoint_id ep`
 * param and its VIDEO_EP_OUT/ALL validation branch.
 */
static int isp_enqueue(const struct device *dev, struct video_buffer *buf)
{
	struct isp_data *data = dev->data;
	uint32_t tmp;

	/* Check if the buffer is 8-byte aligned or not */
	tmp = (uint32_t)buf->buffer;
	if (ROUND_UP(tmp, 8) != tmp) {
		LOG_ERR("Video Buffer is not aligned to 8-byte boundary."
			"It can result in corruption of captured image.");
		return -ENOBUFS;
	}

	buf->bytesused = 0;

	k_fifo_put(&data->fifo_in, buf);

	LOG_DBG("Enqueued buffer: Addr - 0x%x, size - %d, bytesused - %d",
		(uint32_t)buf->buffer, buf->size, buf->bytesused);

	(void)sys_cache_data_flush_and_invd_range(buf->buffer, buf->size);

	return 0;
}

/* v4.4 video-API shim (Alp Lab AB): dropped the `enum video_endpoint_id ep`
 * param and its VIDEO_EP_OUT/ALL validation branch.
 */
static int isp_dequeue(const struct device *dev,
		       struct video_buffer **buf, k_timeout_t timeout)
{
	struct isp_data *data = dev->data;

	struct channel_parameters *channel = &data->init_cfg.channel;

	*buf = k_fifo_get(&data->fifo_out, timeout);
	if (!(*buf)) {
		return -EAGAIN;
	}

	(*buf)->bytesused = channel->output_fmt.pitch * channel->output_fmt.height;
	LOG_DBG("Dequeued buffer: Addr - 0x%08x, size - %d, bytesused - %d",
		(uint32_t)(*buf)->buffer, (*buf)->size, (*buf)->bytesused);
	return 0;
}

#ifdef CONFIG_POLL
/* v4.4 video-API shim (Alp Lab AB): dropped the `enum video_endpoint_id ep`
 * param.
 */
static int isp_set_signal(const struct device *dev,
		struct k_poll_signal *signal)
{
	struct isp_data *data = dev->data;

	if (signal && data->signal) {
		return -EALREADY;
	}
	data->signal = signal;

	return 0;
}
#endif /* CONFIG_POLL */

/*
 * v4.4 video-API shim (Alp Lab AB): the fork's value-pointer ctrl API
 * (set_ctrl/get_ctrl taking `unsigned int cid, void *value`) is gone.  The ISP
 * exposed two PRIVATE CIDs by reading/writing the caller's `void *value`:
 *   - VIDEO_CID_ALIF_ISP_SET -- value = pointer to a struct isp_params, pushed
 *     into the libisp middleware via isp_vsi_set_param();
 *   - VIDEO_CID_ALIF_ISP_GET -- value = pointer to a struct isp_params, read
 *     back via isp_vsi_get_param(),
 * and otherwise forwarded the CID/value to the controller.  v4.4 routes control
 * values through the framework's per-device control registry (video_init_ctrl +
 * struct video_control), with NO value pointer in the
 * .set_ctrl(dev, cid) / .get_volatile_ctrl(dev, cid) callbacks -- so the
 * value-pointer ISP-param path has no v4.4 equivalent.  Until those controls
 * are registered on the control registry, the ctrl callbacks are dropped from
 * the API table below (deferred to the control-registry wiring).  This also
 * removes the only callers of isp_vsi_set_param / isp_vsi_get_param, which is
 * intentional: those symbols are NOT exported by the locally vendored hal_alif
 * libisp wrapper (see the HAL_ALIF VERSION MISMATCH note at the top of this
 * file).  Do NOT fabricate the missing API.
 */
static DEVICE_API(video, isp_driver_api) = {
	.set_format = isp_set_fmt,
	.get_format = isp_get_fmt,
	.set_stream = isp_set_stream,
	.get_caps = isp_get_caps,
	.flush = isp_flush,
	.enqueue = isp_enqueue,
	.dequeue = isp_dequeue,
#ifdef CONFIG_POLL
	.set_signal = isp_set_signal,
#endif /* CONFIG_POLL */
};

int z_impl_isp_vsi_register_ae_status_callback(const struct device *dev,
		isp_ae_status_cb ae_status_cb, void *user_data)
{
	struct isp_data *data = dev->data;

	data->init_cfg.ae_status_cb = ae_status_cb;
	data->init_cfg.ae_status_user_data = user_data;

	return 0;
}

#ifdef CONFIG_USERSPACE
#include <zephyr/internal/syscall_handler.h>
static int z_vrfy_isp_vsi_register_ae_status_callback(const struct device *dev,
		isp_ae_status_cb ae_status_cb, void *user_data)
{
	K_OOPS(K_SYSCALL_SPECIFIC_DRIVER(dev, K_OBJ_DRIVER_VIDEO, &isp_driver_api));
	return z_impl_isp_vsi_register_ae_status_callback(dev, ae_status_cb, user_data);
}
#include <zephyr/syscalls/register_ae_status_callback_mrsh.c>
#endif /* CONFIG_USERSPACE */

static int isp_configure(const struct device *dev)
{
	const struct isp_config *config = dev->config;
	struct isp_data *data = dev->data;

	struct port_parameters *port = &data->init_cfg.port;
	int ret;

	ret = isp_vsi_init(&data->init_cfg);
	if (ret) {
		LOG_ERR("Failed to Init ISP device!");
		return ret;
	}

	if (config->tpg_img_idx == IMG_DISABLED) {
		port->input = INPUT_SENSOR;
	} else {
		port->input = INPUT_TPG;
		switch (config->tpg_pix_width) {
		case TPG_BIT_WIDTH_8:
			if (config->tpg_bayer_pattern == RGGB) {
				port->port_fmt.pixelformat = VIDEO_PIX_FMT_RGGB8;
			} else if (config->tpg_bayer_pattern == GRBG) {
				port->port_fmt.pixelformat = VIDEO_PIX_FMT_GRBG8;
			} else if (config->tpg_bayer_pattern == GBRG) {
				port->port_fmt.pixelformat = VIDEO_PIX_FMT_GBRG8;
			} else if (config->tpg_bayer_pattern == BGGR) {
				port->port_fmt.pixelformat = VIDEO_PIX_FMT_BGGR8;
			}
			break;
		case TPG_BIT_WIDTH_10:
			if (config->tpg_bayer_pattern == RGGB) {
				port->port_fmt.pixelformat = VIDEO_PIX_FMT_RGGB10;
			} else if (config->tpg_bayer_pattern == GRBG) {
				port->port_fmt.pixelformat = VIDEO_PIX_FMT_GRBG10;
			} else if (config->tpg_bayer_pattern == GBRG) {
				port->port_fmt.pixelformat = VIDEO_PIX_FMT_GBRG10;
			} else if (config->tpg_bayer_pattern == BGGR) {
				port->port_fmt.pixelformat = VIDEO_PIX_FMT_BGGR10;
			}
			break;
		case TPG_BIT_WIDTH_12:
			if (config->tpg_bayer_pattern == RGGB) {
				port->port_fmt.pixelformat = VIDEO_PIX_FMT_RGGB12;
			} else if (config->tpg_bayer_pattern == GRBG) {
				port->port_fmt.pixelformat = VIDEO_PIX_FMT_GRBG12;
			} else if (config->tpg_bayer_pattern == GBRG) {
				port->port_fmt.pixelformat = VIDEO_PIX_FMT_GBRG12;
			} else if (config->tpg_bayer_pattern == BGGR) {
				port->port_fmt.pixelformat = VIDEO_PIX_FMT_BGGR12;
			}
			break;
		default:
			LOG_ERR("Unknown bit width!");
			return -EINVAL;
		}
		port->tpg_image_idx = config->tpg_img_idx;
	}

	port->hdr = LINEAR;

	return 0;
}

int video_isp_init(const struct device *dev)
{
	const struct isp_config *config = dev->config;
	struct isp_data *data = dev->data;
	int ret;

	if (!config->controller && config->tpg_img_idx == IMG_DISABLED) {
		LOG_ERR("Both Camera controller and TPG are not enabled!");
		return -ENODEV;
	}

	DEVICE_MMIO_MAP(dev, K_MEM_CACHE_NONE);
	LOG_DBG("MMIO Address: 0x%x", (uint32_t) DEVICE_MMIO_GET(dev));

	/*
	 * Setup the ISR callback work.
	 */
	k_work_init(&data->cb_work, isp_cb_work);
	k_work_queue_init(&data->cb_workq);
	k_work_queue_start(&data->cb_workq, isp_cb_workq, K_KERNEL_STACK_SIZEOF(isp_cb_workq),
			   K_PRIO_COOP(WORKQ_PRIORITY), NULL);
	k_thread_name_set(&data->cb_workq.thread, "isp_work_helper");

	/*
	 * Setup interrupts.
	 */
	config->irq_config_func(dev);

	/*
	 * Setup FIFO for ISP driver.
	 */
	k_fifo_init(&data->fifo_in);
	k_fifo_init(&data->fifo_out);
	data->dev = dev;

	/*
	 * Do ISP configuration.
	 */
	ret = isp_configure(dev);
	if (ret) {
		LOG_ERR("Failed to configure the ISP!");
		return ret;
	}

	LOG_DBG("ISP IRQn: %d MI-ISP IRQn: %d", config->irqn, config->mi_irqn);

	switch (config->tpg_img_idx) {
	case IMG_3X3_COLOR_BLOCK:
		LOG_DBG("TPG Status: 3x3 Color Bar");
		break;
	case IMG_COLOR_BAR:
		LOG_DBG("TPG Status: Color Bar");
		break;
	case IMG_GRAY_BAR:
		LOG_DBG("TPG Status: Gray Bar");
		break;
	case IMG_HIGHLIGHTED_GRID:
		LOG_DBG("TPG Status: Highlighted Grid");
		break;
	case IMG_RANDOM_GENERATOR:
		LOG_DBG("TPG Status: Random Generator");
		break;
	case IMG_DISABLED:
		LOG_DBG("TPG Status: Disabled");
		break;
	default:
		LOG_DBG("Unknown TPG Image format!");
	};

	return 0;
}

#define REMOTE_DEVICE(i, idx)	                                           \
	DT_NODE_REMOTE_DEVICE(DT_INST_ENDPOINT_BY_ID(i, idx, 0))

#define REMOTE_EP(n, pid, epid)                                            \
	DT_NODELABEL(DT_STRING_TOKEN(DT_INST_ENDPOINT_BY_ID(n, pid, epid), \
				remote_endpoint_label))

#define ISP_DEFINE(i)                                                                         \
	static void isp_config_func_##i(const struct device *dev);                            \
	const struct isp_config isp_config_##i = {                                            \
		DEVICE_MMIO_ROM_INIT(DT_DRV_INST(i)),                                         \
		.irq_config_func = isp_config_func_##i,                                       \
		.controller = DEVICE_DT_GET_OR_NULL(REMOTE_DEVICE(i, 0)),                     \
		.tpg_bayer_pattern = DT_INST_ENUM_IDX(i, tpg_bayer_pattern),                  \
		.tpg_img_idx = DT_INST_ENUM_IDX(i, tpg_image_idx),                            \
		.tpg_pix_width = DT_INST_ENUM_IDX_OR(i, tpg_pix_width, 2),                    \
		.irqn = DT_INST_IRQ_BY_NAME(i, isp, irq),                                     \
		.mi_irqn = DT_INST_IRQ_BY_NAME(i, mi_isp, irq),                               \
	};                                                                                    \
                                                                                              \
	struct isp_data isp_data_##i = {                                                      \
		.is_streaming = false,                                                        \
		.init_cfg = {                                                                 \
			.port = {                                                             \
				.mode = DT_INST_ENUM_IDX(i, isp_subsampling),                 \
				.field = DT_INST_ENUM_IDX(i, fieldsel),                       \
                                                                                              \
				.out_form_rect = {                                            \
					.top = DT_INST_PROP(i, crop_y0),                      \
					.left = DT_INST_PROP(i, crop_x0),                     \
					.width = 0,                                           \
					.height = 0,                                          \
				},                                                            \
				.isp_idx = i,                                                 \
				.port_id = 0,                                                 \
			},                                                                    \
			.channel = {                                                          \
				.trans_bus = ONLINE,                                          \
				.output_fmt = {},                                             \
				.channel_idx = 0                                              \
			},                                                                    \
		},                                                                            \
	};                                                                                    \
                                                                                              \
	DEVICE_DT_INST_DEFINE(i,                                                              \
		video_isp_init,                                                               \
		NULL,                                                                         \
		&isp_data_##i,                                                                \
		&isp_config_##i,                                                              \
		POST_KERNEL,                                                                  \
		CONFIG_KERNEL_INIT_PRIORITY_DEVICE,                                           \
		&isp_driver_api);                                                             \
		                                                                              \
	static void isp_config_func_##i(const struct device *dev)                             \
	{                                                                                     \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(i, isp, irq),                                 \
			    DT_INST_IRQ_BY_NAME(i, isp, priority),                            \
			    isp_isr_handler, DEVICE_DT_INST_GET(i), 0);                       \
		irq_enable(DT_INST_IRQ_BY_NAME(i, isp, irq));                                 \
		                                                                              \
		IRQ_CONNECT(DT_INST_IRQ_BY_NAME(i, mi_isp, irq),                              \
			    DT_INST_IRQ_BY_NAME(i, mi_isp, priority),                         \
			    isp_isr_handler, DEVICE_DT_INST_GET(i), 0);                       \
		irq_enable(DT_INST_IRQ_BY_NAME(i, mi_isp, irq));                              \
	}

DT_INST_FOREACH_STATUS_OKAY(ISP_DEFINE)
