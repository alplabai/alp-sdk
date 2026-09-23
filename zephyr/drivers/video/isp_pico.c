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
 * COMPILES + LINKS against hal_alif v2.3.0 (west.yml pinned): the two headers
 * this TU needed, <zephyr/drivers/video/isp-vsi.h> and its
 * isp_ctrl_params.h, are vendored VERBATIM (Apache-2.0) into
 * zephyr/include/zephyr/drivers/video/ -- see the HAL_ALIF note below.
 * Build-only proof: examples/aen/aen-isp-regcheck (AEN801/E8).  RUNTIME:
 * PROVEN on real silicon through a full camera->csi->isp->memory
 * media-controller graph, a real OV5647 sensor over CSI-2, with AE/AWB
 * running -- see examples/aen/aen-isp-ov5647-capture.
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
 *   - the value-pointer .set_ctrl/.get_ctrl callbacks (fork's `unsigned int
 *     cid, void *value` form) are gone -- v4.4's registry-based .set_ctrl
 *     (this device, uint32_t cid) is what's wired up now (isp_set_ctrl(),
 *     below), reaching isp_vsi_set_param/isp_vsi_get_param through
 *     video_init_ctrl() + this device's VIDEO_DEVICE_DEFINE chain.
 *
 * !!! HAL_ALIF VERSION MISMATCH -- RESOLVED (was FLAGGED) !!!
 * This 2026 isp_pico.c was authored against a NEWER hal_alif libisp wrapper than
 * the one that used to be vendored locally (modules/hal/alif/drivers/isp/isp_wrapper,
 * 2025).  west.yml now pins hal_alif v2.3.0, whose isp_wrapper/src/isp_api_wrapper.c
 * itself #includes <zephyr/drivers/video/isp-vsi.h> -- so the missing-header
 * blocker is upstream's too, and it ships a 3-arg
 * isp_vsi_bottom_half(dev, init_cfg, mi_mis) that already matches this driver's
 * call site (isp_bottom_half(), below).  The remaining gap -- isp-vsi.h itself
 * was never published by hal_alif -- is closed by vendoring it (+ its
 * isp_ctrl_params.h dependency) VERBATIM (Apache-2.0) from
 * alifsemi/zephyr_alif @ v2.3.0 into zephyr/include/zephyr/drivers/video/.  The
 * v2.3.0 wrapper exports isp_vsi_init/update_cfg/uninit/bottom_half/start/stop/
 * enqueue/dequeue -- everything this port calls -- PLUS isp_vsi_set_param/
 * isp_vsi_get_param (isp_api_wrapper.c:880, :1242), now reachable through
 * isp_set_ctrl()'s VIDEO_CID_AUTO_WHITE_BALANCE/VIDEO_CID_EXPOSURE_AUTO
 * cases, below.  Do NOT fabricate any hal_alif API.
 * COMPILE + LINK + RUNTIME: PROVEN on real silicon (examples/aen/
 * aen-isp-ov5647-capture, CONFIG_VIDEO_ISP_VSI=y).
 * vendor-ext, ISP=Vivante blob (opt-in).
 */
#define DT_DRV_COMPAT vsi_isp_pico

#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(ISP, CONFIG_VIDEO_LOG_LEVEL);

#include <zephyr/drivers/video.h>
#include <zephyr/drivers/video-controls.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/device_mmio.h>
#include <zephyr/drivers/pinctrl.h>

#include "isp_pico.h"
#include <zephyr/drivers/video/isp_frame_size.h>
#include <zephyr/drivers/video/video_alif.h>
#include <soc_memory_map.h>
#include <zephyr/cache.h>
/* Upstream's private drivers/video/video_device.h (put on the include path by
 * zephyr/CMakeLists.txt's ${ZEPHYR_BASE}/drivers/video dir) -- needed for
 * VIDEO_DEVICE_DEFINE, below, so v4.4's control-registry walk
 * (video_find_ctrl(), drivers/video/video_ctrls.c) can chain from this
 * device to its upstream controller. */
#include "video_device.h"

/*
 * alp-sdk ABI enforcement (Alp Lab AB): the hal_alif prebuilt ISP middleware
 * library (Lib/libisp_gcc.a, linked in via CONFIG_USE_ALIF_ISP_LIB -- required
 * for this driver to link at all) is built hard-float (VFP register args),
 * same as the JPEG SW helper lib.  Zephyr's Cortex-M FP ABI (FP_HARDABI vs
 * FP_SOFTABI) is a `choice` member under FPU, not a plain bool, so it cannot be
 * `select`ed from Kconfig (see zephyr/kconfigs/vendor-alif-peripherals.kconfig,
 * right after the VIDEO_ISP_VSI config). Catch a missing hard-float ABI here,
 * at compile time, with a legible message -- instead of leaving the user to
 * decode a bare "uses VFP register arguments, zephyr_pre0.elf does not"
 * linker error.
 */
#if defined(CONFIG_USE_ALIF_ISP_LIB)
BUILD_ASSERT(IS_ENABLED(CONFIG_FP_HARDABI),
	     "CONFIG_VIDEO_ISP_VSI + CONFIG_USE_ALIF_ISP_LIB link the hal_alif prebuilt "
	     "Lib/libisp_gcc.a, which is hard-float (VFP register arguments). Set "
	     "CONFIG_FP_HARDABI=y (the \"Floating point ABI\" choice, under FPU) or the "
	     "final link fails.");

/*
 * alp-sdk glue (Alp Lab AB): modules/hal/alif/drivers/isp/isp_wrapper/src/
 * isp_api_wrapper.c declares `extern int log_level(void);` and takes its
 * address for VsiLogLevelSet() (the wrapper's own public log API,
 * inc/lib/vsios_log.h) -- that header's contract is for the CALLER to supply
 * this getter; hal_alif ships no definition anywhere in the module, so
 * without it the final link fails with an undefined reference.  This is a
 * direct pass-through, not new logic: VsiLogLevel_t (vsios_log.h) numbers its
 * levels identically to Zephyr's LOG_LEVEL_* for every level both define
 * (NONE=0, ERR=1, WARN/WRN=2, INFO/INF=3, DEBUG/DBG=4).  CONFIG_LOG_DEFAULT_LEVEL
 * only exists in autoconf.h when CONFIG_LOG=y (it lives inside an `if LOG`
 * block, subsys/logging/Kconfig.filtering) -- guard with #if defined() rather
 * than assume it, so this stays correct for the CONFIG_LOG=n RAM-run configs
 * this SDK's AEN regchecks use (see aen-isp-regcheck/prj.conf).
 */
int log_level(void)
{
#if defined(CONFIG_LOG_DEFAULT_LEVEL)
	return CONFIG_LOG_DEFAULT_LEVEL;
#else
	return 1; /* VSI_LOG_LEVEL_ERR -- logging subsystem compiled out */
#endif
}
#endif

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
	/*
	 * VIDEO_PIX_FMT_SBGGR10P (packed) -- run 72: examples/aen/
	 * aen-isp-ov5647-capture's stage 3d/4 diagnostic requests this
	 * exactly (mirroring what the OV5647 driver advertises and the CPI
	 * negotiates on the CSI wire), matching the hal_alif
	 * 0003-isp-add-sbggr10p-bggr10-input-mapping.patch wrapper mapping
	 * added for it -- but this table, the ISP driver's OWN INPUT format
	 * gate, had no case for it, so find_format() rejected it with
	 * -ENOTSUP before isp_vsi_update_cfg() (and the wrapper's new
	 * mapping) was ever reached.  See bayer_sample_depth(), below,
	 * which already keys the correct PIN_MAPPING=1 (10-bit) off this
	 * same fourcc.
	 */
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_SBGGR10P, 1920, 1080),
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

/*
 * OUTPUT (MI/channel) formats -- what isp_pixelfmt_from_fourcc()
 * (isp_api_wrapper.c) can actually map to a libisp PIXEL_FORMAT_E the MI
 * hardware will accept for a CHANNEL (VSI_MPI_ISP_SetChnAttr).
 *
 * Bench evidence (run 66, aen-isp-regcheck stage 1): setting OUTPUT format
 * VIDEO_PIX_FMT_SBGGR10 (1280x720) made isp_vsi_update_cfg() fail with -22,
 * logged as "E: Setting the Channel config failed!" from
 * VSI_MPI_ISP_SetChnAttr.  Root cause (read from vsi_comm_video.h, isp_wrapper
 * /inc/lib/): the library has TWO SEPARATE pixel-format namespaces -- Bayer
 * PIXEL_FORMAT_{BGGR,GBRG,GRBG,RGGB}{8,10,12,14,16} (input-port-only, what
 * the SENSOR/TPG transmits) vs. PIXEL_FORMAT_{RAW8,RAW10,RAW12}/YUV.../RGB888
 * (output-channel-capable, what the MI can write to memory).
 * isp_pixelfmt_from_fourcc() uses ONE switch for BOTH port->port_fmt
 * (isp_vsi_update_cfg's port attrs) and channel->output_fmt (its channel
 * attrs), and maps every VIDEO_PIX_FMT_{B,G}*{8,10,12,...} straight to the
 * matching Bayer PIXEL_FORMAT_* -- valid for the port, REJECTED by
 * SetChnAttr for the channel.  So the fourccs below are dropped from this
 * list because they map to an input-only Bayer enum:
 *   BGGR/GBRG/GRBG/RGGB {8,10,12} (isp_pixelfmt_from_fourcc:201-224)
 * ...and these are dropped because they map to -1 (unhandled -> "Unsupported
 * Channel pixel format!", isp_api_wrapper.c:577-580) or to a Bayer enum via a
 * TODO hack instead of the RAW* family:
 *   VIDEO_PIX_FMT_Y10, VIDEO_PIX_FMT_Y12 (no case in isp_pixelfmt_from_fourcc
 *     -> -1), VIDEO_PIX_FMT_Y10P (maps to PIXEL_FORMAT_GRBG10, a documented
 *     TODO placeholder pending a real RAW10 mapping -- see the wrapper fix
 *     note above isp_stream_start(), below).
 * What remains is exactly what isp_pixelfmt_from_fourcc() maps to a
 * genuinely output-capable PIXEL_FORMAT_E: YUYV/VYUY/UYVY (packed YUV422,
 * Alif's own DFP default MI output -- RTE_Device.h RTE_ISP_OUTPUT_FORMAT=32
 * == PIXEL_FORMAT_YUYV), NV12/NV21/NV16/NV61 (semi-planar YUV),
 * YUV422P/YUV420 (planar YUV), GREY (-> PIXEL_FORMAT_RAW8), Y12P
 * (-> PIXEL_FORMAT_RAW12), RGB888_PLANAR_PRIVATE.
 */
static const struct video_format_cap supported_output_fmts[] = {
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_NV12, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_NV21, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_NV16, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_NV61, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_YUV422P, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_YUV420, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_YUYV, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_VYUY, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_UYVY, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_GREY, 1920, 1080),
	ISP_VIDEO_FORMAT_CAP(VIDEO_PIX_FMT_Y12P, 1920, 1080),
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

	/* A frame-end with no stream running (latched before this boot, or
	 * landing after a stop) has no buffer to complete and must not reach
	 * the VSI lib, which may not even be configured yet.
	 */
	if (!data->is_streaming) {
		return;
	}

	/* Do bottom half processing of all the modules at the end of frame.
	 * Same lib_lock isp_apply_wb()/isp_apply_ae() take -- this runs on
	 * isp_cb_workq, those run from isp_stream_start() on the caller's
	 * thread; see struct isp_data's lib_lock comment (isp_pico.h). */
	k_mutex_lock(&data->lib_lock, K_FOREVER);
	isp_vsi_bottom_half(dev, &data->init_cfg, data->mi_mis);
	k_mutex_unlock(&data->lib_lock);

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
		/*
		 * In TPG mode config->controller is a legitimate NULL (see
		 * video_isp_init()'s init-time check) -- there is no endpoint
		 * device to forward stream-stop to; skip it rather than call
		 * video_stream_stop(NULL, ...) (harmless -- its own dev==NULL
		 * guard returns -EINVAL without touching *dev -- but dead code
		 * whose ignored return obscures that this path does nothing
		 * useful in TPG mode).
		 * v4.4 video-API shim (Alp Lab AB): video_stream_stop gained an
		 * `enum video_buf_type`; the controller is the capture source ->
		 * VIDEO_BUF_TYPE_OUTPUT.
		 */
		if (config->controller) {
			video_stream_stop(config->controller, VIDEO_BUF_TYPE_OUTPUT);
		}
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

/*
 * Bench instrumentation (Alp Lab AB), CONFIG_VIDEO_ISP_VSI_FRAME_STATS
 * (default n -- review round, post-3511cd180): counts MI_INTR_MP_FRAME_END
 * events -- i.e. IRQ 368 ("mi-isp") actually firing a completed-frame
 * interrupt, not just the shared isp_isr_handler() being entered for one of
 * its other status bits.  Global (not static) + volatile so a RAM-run bench
 * app can read it with an `extern` declaration, or a debugger can read the
 * symbol directly over SWD post-mortem.
 */
#ifdef CONFIG_VIDEO_ISP_VSI_FRAME_STATS
volatile uint32_t isp_mi_frame_end_count;
#endif

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
#ifdef CONFIG_VIDEO_ISP_VSI_FRAME_STATS
		isp_mi_frame_end_count++;
#endif
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

		/*
		 * find_format() rejects with -ENOTSUP right here for any fourcc
		 * not in supported_output_fmts[] -- in particular every Bayer
		 * format (BGGR/GBRG/GRBG/RGGB), which the libisp wrapper CAN set
		 * on the port (input) but NOT on the channel (output; run 66 --
		 * see the comment above supported_output_fmts[]).  This is the
		 * fail-fast point for that class of error: catching it here means
		 * an app misconfiguring the output format finds out from
		 * video_set_format() immediately, not from a cryptic -EINVAL out
		 * of isp_stream_start() -> isp_vsi_update_cfg() three calls later.
		 */
		ret = find_format(fmt, supported_output_fmts);
		if (ret) {
			LOG_ERR("Desired format is not supported by the ISP Output EP!");
			return ret;
		}

		/*
		 * isp_apply_mrsz() divides by (height - 1) to scale 4:2:0
		 * chroma; find_format() above ignores height_step (ISP_VIDEO_
		 * FORMAT_CAP sets .height_step = 4), so it lets a height < 4
		 * or odd height through: height 1 traps DIV_0_TRP, height 2
		 * gives a degenerate SCALE_VC=0, and an odd height gives a
		 * non-integer 2:1 chroma downscale. Reject here, at the
		 * fail-fast point this switch already uses for output-format
		 * errors.
		 */
		if ((fmt->pixelformat == VIDEO_PIX_FMT_YUV420 || fmt->pixelformat == VIDEO_PIX_FMT_NV12 ||
		     fmt->pixelformat == VIDEO_PIX_FMT_NV21) &&
		    (fmt->height < 4 || (fmt->height % 2) != 0)) {
			LOG_ERR("4:2:0 output height %u must be even and >= 4!", fmt->height);
			return -EINVAL;
		}

		/*
		 * A caller that doesn't already know its own stride (every
		 * backend negotiating a fresh format) passes pitch == 0 and
		 * expects this driver to fill it in -- video_set_format()'s
		 * fmt is in/out for exactly this reason (see
		 * video_stm32_venc.c's stm32_venc_set_fmt() for the same
		 * convention upstream). alp_isp_default_pitch()
		 * (isp_frame_size.h) is the single place this driver AND
		 * src/backends/camera/alif_isp_pico.c's buffer-pool sizing
		 * derive a format's byte geometry from -- see that header for
		 * why planar/semi-planar YUV gets the LUMA-only line stride,
		 * not an average-bpp-derived one.
		 */
		if (fmt->pitch == 0u) {
			fmt->pitch = alp_isp_default_pitch(fmt->pixelformat, fmt->width);
			if (fmt->pitch == 0u) {
				LOG_ERR("Cannot derive a pitch for this output fourcc!");
				return -EINVAL;
			}
		}

		channel->output_fmt = *fmt;
		break;
	default:
		LOG_ERR("Unsupported buffer type!");
		return -EINVAL;

	}

	return 0;
}

/*
 * Review round (post-3511cd180), item "sensor-specific constants in the
 * generic ISP driver": isp_apply_wb()/isp_apply_ae(), below, are written to
 * stay usable for whatever sensor is bound, not just the OV5647 this branch
 * bring-up happens to target.
 *
 * AWB: instead of this driver carrying its own calibration table (a prior
 * revision borrowed Alif's ARX3A0 reference table -- wrong sensor, and a
 * generic driver has no business embedding ANY sensor's calibration data),
 * isp_apply_wb() now round-trips isp_vsi_get_param()/isp_vsi_set_param():
 * read back whatever calibration isp_vsi_update_cfg()'s SetCalib() already
 * loaded (isp_param_conf.h, selected at BUILD time by which
 * CONFIG_ISP_LIB_*_MODULE Kconfigs + calibration table the integrator
 * compiles in -- not this driver's concern), flip only .enable/.op_mode,
 * write it back unchanged otherwise. AWB now defaults ON
 * (isp_init_controls()'s .def=1, matching the calibration's own
 * OP_TYPE_AUTO AWB) -- the calling example can still turn it off, this
 * driver doesn't presume a sensor needs it disabled.
 *
 * AE: int_time_min/max are derived from video_get_frmival() (the sensor's
 * OWN reported frame period, via the isp -> cam -> csi -> sensor chain) --
 * sensor-agnostic, no register facts needed. again_min/max are derived from
 * video_query_ctrl(VIDEO_CID_ANALOGUE_GAIN) (the sensor's OWN registered
 * gain-control range) converted to this library's fixed-point scale via
 * ISP_AE_GAIN_REG_PER_1X, below -- that ONE conversion factor (a sensor's
 * analogue-gain register isn't standardized to any particular "1x" value by
 * Zephyr's video API) is the one piece of this file that stays sensor-
 * specific by construction, same status as hal_alif patch 0005's matching
 * write-back scaling (isp_api_wrapper.c) -- both cite OV5647_AGC_GAIN
 * (0x350a) register value 0x10 = 1.0x and are marked upstreamable: false in
 * patches.yml for exactly this reason. If a future sensor's "1x" register
 * value differs, THIS constant (and 0005's matching one) are what to change.
 */
#define ISP_AE_GAIN_REG_PER_1X 16 /* OV5647 AGC_GAIN (0x350a): reg 0x10 = 1.0x */

/*
 * Run 74 established the proven-working order: apply AFTER isp_vsi_start()'s
 * Enable*, not before -- at the time, isp_vsi_update_cfg()'s SetCalib()
 * (isp_api_wrapper.c) reloaded WB/AE from isp_param_conf.h on EVERY restart,
 * so anything applied before Enable* was simply overwritten by it. Patch
 * 0007 (zephyr/patches/hal_alif/0007-isp-setcalib-load-once.patch) changed
 * that: SetCalib in isp_vsi_update_cfg() is now guarded by a once-flag (see
 * the run-85 comment above isp_apply_aem_wbm()), so it only fires on the
 * FIRST isp_vsi_update_cfg() call, never again on a later restart. A
 * runtime isp_vsi_set_param() therefore now persists across restarts on its
 * own; run 74's apply-after-Enable* ordering remains correct (still needed
 * for the FIRST apply to stick), it just no longer needs re-applying every
 * time.
 *
 * Patch 0009 (zephyr/patches/hal_alif/0009-isp-setcalib-before-3a-callbacks.
 * patch) fixed the real bug behind "AWB/AE never move": the pinned hal_alif
 * registered the AWB/AE callbacks BEFORE SetCalib. With it, the
 * calibration's own OP_TYPE_AUTO AE+AWB converge from the FIRST stream with
 * no runtime isp_vsi_set_param() and no restart at all (bench runs 153,
 * 156) -- so isp_stream_start() (the call site, below) only calls
 * isp_apply_wb()/isp_apply_ae() when wb_dirty/ae_dirty is set
 * (isp_set_ctrl()'s dirty flags, isp_pico.h): any ctrl change sets the
 * flag, and ae_dirty additionally starts true (isp_init_controls()) so the
 * AE limits this sensor's calibration doesn't carry still get pushed at
 * the first stream start too. Patch 0009 also adds its OWN unconditional
 * SetCalib call at isp_vsi_init() (init time), so SetCalib now runs at most
 * TWICE per boot -- once from patch 0009 at init, once from patch 0007's
 * once-guard at the first isp_vsi_update_cfg() -- never on a later restart.
 */
static int isp_apply_wb(const struct device *dev, bool enable)
{
	struct isp_data *data = dev->data;
	struct isp_params params = {0};
	int ret;

	if (!IS_ENABLED(CONFIG_ISP_LIB_WB_MODULE)) {
		return 0;
	}

	k_mutex_lock(&data->lib_lock, K_FOREVER);

	/*
	 * Run 90-106 bisect: isp_vsi_get_param()'s wrapper implementation
	 * (isp_api_wrapper.c) gates EACH block on `params->valid_mask &
	 * ISP_PARAM_MASK_*` -- the SAME mask the caller is expected to set
	 * BEFORE the call, not just before the matching set_param(). This
	 * used to be left at 0 here (params = {0}, valid_mask set only AFTER
	 * the get, for the SET below), so the get silently populated
	 * NOTHING -- params.wb stayed all-zero from the local initializer.
	 * The round-trip's whole POINT (preserve run_interval/speed/
	 * tolerance/init_color_temp/calib from whatever SetCalib loaded) was
	 * therefore a no-op: this function pushed enable=1/AUTO with a
	 * ZEROED calib table and zeroed convergence tuning -- poisoned AWB,
	 * not a working one. Fix: set valid_mask BEFORE the get, so the
	 * round-trip is real.
	 */
	params.valid_mask = ISP_PARAM_MASK_WB;

	ret = isp_vsi_get_param(&data->init_cfg, &params);
	if (ret) {
		k_mutex_unlock(&data->lib_lock);
		LOG_ERR("Failed to read back WB/calib state: %d", ret);
		return ret;
	}

	/* Run 107 observability: what did the round-trip ACTUALLY read back?
	 * illum[ISP_ILLUMINANT_D50] (index 3) -- run 106/74's hand-built
	 * struct used init_color_temp=5000 (D50), the same illuminant this
	 * logs. */
	LOG_DBG("WB GET rc=%d enable=%u op_mode=%u run_interval=%u speed=%u "
		"tolerance=%u init_color_temp=%u calib.rg_min=%d rg_max=%d "
		"illum[D50] temp=%u r=0x%x gr=0x%x gb=0x%x b=0x%x",
		ret,
		params.wb.enable,
		params.wb.op_mode,
		params.wb.run_interval,
		params.wb.speed,
		params.wb.tolerance,
		params.wb.init_color_temp,
		params.wb.calib.rg_min,
		params.wb.calib.rg_max,
		params.wb.calib.illuminant[ISP_ILLUMINANT_D50].color_temp,
		params.wb.calib.illuminant[ISP_ILLUMINANT_D50].r_gain,
		params.wb.calib.illuminant[ISP_ILLUMINANT_D50].gr_gain,
		params.wb.calib.illuminant[ISP_ILLUMINANT_D50].gb_gain,
		params.wb.calib.illuminant[ISP_ILLUMINANT_D50].b_gain);

	params.wb.enable   = enable ? 1 : 0;
	params.wb.op_mode  = ISP_OP_AUTO;

	/* Run 84: VIDEO_ISP_VSI_WB_MANUAL_GAIN diagnostic escape hatch (see the
	 * Kconfig help) -- forces specific gains instead of round-tripping
	 * whatever SetCalib()'s .wb default loaded, to isolate "is the colour
	 * pipeline downstream of WB correct" from "has AWB itself converged".
	 */
#if defined(CONFIG_VIDEO_ISP_VSI_WB_MANUAL_GAIN)
	/* #if, not IS_ENABLED(): the four VIDEO_ISP_VSI_WB_GAIN_* symbols
	 * below only exist in Kconfig's output (autoconf.h) when this parent
	 * bool is set (each `depends on VIDEO_ISP_VSI_WB_MANUAL_GAIN`) -- an
	 * IS_ENABLED() runtime guard still requires the identifiers to be
	 * DECLARED for the compiler even in the dead branch; only a
	 * preprocessor #if drops the reference entirely when they don't
	 * exist.
	 */
	params.wb.op_mode  = ISP_OP_MANUAL;
	params.wb.r_gain   = CONFIG_VIDEO_ISP_VSI_WB_GAIN_R;
	params.wb.gr_gain  = CONFIG_VIDEO_ISP_VSI_WB_GAIN_GR;
	params.wb.gb_gain  = CONFIG_VIDEO_ISP_VSI_WB_GAIN_GB;
	params.wb.b_gain   = CONFIG_VIDEO_ISP_VSI_WB_GAIN_B;
#endif /* defined(CONFIG_VIDEO_ISP_VSI_WB_MANUAL_GAIN) */

	ret = isp_vsi_set_param(&data->init_cfg, &params);

	k_mutex_unlock(&data->lib_lock);

	LOG_DBG("WB SET enable=%u op_mode=%u run_interval=%u speed=%u tolerance=%u "
		"init_color_temp=%u rc=%d",
		params.wb.enable,
		params.wb.op_mode,
		params.wb.run_interval,
		params.wb.speed,
		params.wb.tolerance,
		params.wb.init_color_temp,
		ret);

	if (ret) {
		LOG_ERR("Failed to %s AWB: %d", enable ? "enable" : "disable", ret);
	}

	return ret;
}

/*
 * Run 90-107 bisect (bench run 107's "Control id 0x9e0903 is inactive" /
 * "Failed to write Total Gain to the sensor!" regression): split out of
 * isp_apply_ae() -- the SENSOR-side gating below (force the sensor's OWN
 * AEC/AUTOGAIN out of the way) must happen BEFORE the ISP starts capturing
 * frames at all, not deferred with the library-side isp_vsi_set_param(AE)
 * push (isp_apply_ae(), below). The deferred apply only runs AFTER at
 * least one frame-end + the settle delay; isp_vsi_bottom_half()'s own
 * write-back (hal_alif patch 0005) can attempt a VIDEO_CID_ANALOGUE_GAIN
 * write on the VERY FIRST frame-end, before that deferred work has had a
 * chance to turn the sensor's own AUTOGAIN off -- ov5647.c's
 * video_auto_cluster_ctrl() clusters gain under autogain, so a write while
 * autogain is still on is rejected as "inactive" (the same failure mode
 * bench run 76 first found, just from a different cause this time: missing
 * synchronisation, not a missing call). Called synchronously from
 * isp_stream_start(), BEFORE isp_vsi_start()'s Enable* -- so the sensor is
 * already gated before the first frame, hence the first frame-end, can
 * happen at all. Unconditional every restart, including the FIRST one
 * (video_set_ctrl()'s own registry, video_ctrls.c, already no-ops a write
 * whose value hasn't changed): patch 0009 registers the calibration's
 * OP_TYPE_AUTO AE before SetCalib, so library AE is already running from
 * the first stream regardless of this driver's ctrl default.
 *
 * Always gates the sensor to MANUAL (EXPOSURE_AUTO=MANUAL, AUTOGAIN=0),
 * regardless of which mode the library's own AE module is running in: the
 * ISP -- never the sensor's own AEC/AUTOGAIN -- owns exposure/gain either
 * way. With the lib in AUTO it computes exposure/gain and writes them to
 * the sensor; with the lib in MANUAL (isp_apply_ae(false), below) it writes
 * its OWN manual values to the sensor instead. A gate that handed the
 * sensor back to its own AUTO for library-MANUAL left the sensor's gain
 * control INACTIVE for patch 0005's per-frame write-back ("Control id
 * 0x9e0903 is inactive" logged every frame).
 */
static void isp_apply_ae_sensor_gate(const struct device *dev)
{
	const struct isp_config *config = dev->config;

	if (!IS_ENABLED(CONFIG_ISP_LIB_AE_MODULE) || !config->controller) {
		return;
	}

	struct video_control sensor_exp_auto = {
		.id  = VIDEO_CID_EXPOSURE_AUTO,
		.val = VIDEO_EXPOSURE_MANUAL,
	};
	struct video_control sensor_autogain = {
		.id  = VIDEO_CID_AUTOGAIN,
		.val = 0,
	};
	int rc;

	rc = video_set_ctrl(config->controller, &sensor_exp_auto);
	if (rc) {
		LOG_WRN("Failed to set sensor EXPOSURE_AUTO: %d", rc);
	}
	rc = video_set_ctrl(config->controller, &sensor_autogain);
	if (rc) {
		LOG_WRN("Failed to set sensor AUTOGAIN: %d", rc);
	}
}

static int isp_apply_ae(const struct device *dev, bool enable)
{
	const struct isp_config *config = dev->config;
	struct isp_data *data = dev->data;
	struct isp_params params = {0};
	/* 15 fps / OV5647_AGC_GAIN-range fallback -- see the block comment
	 * above for why this is the one sensor-tuned constant left, and only
	 * as a fallback for when the live queries below fail. 2095 lines
	 * (VTS - 4, 15 fps) * 31749 ns/line (OV5647_HTS_640X480_BINNED /
	 * pixel_rate, bench run 61) = 66514155 ns.
	 */
	uint32_t int_time_max_us = 66514;
	uint32_t again_min = 1024;   /* library units, 1x = 1024 */
	uint32_t again_max = 16368;  /* (1023 OV5647 AGC_GAIN max) * 1024 / 16 */

	if (!IS_ENABLED(CONFIG_ISP_LIB_AE_MODULE)) {
		return 0;
	}

	if (config->controller) {
		/*
		 * Run for BOTH modes, not just enable=true: manual mode
		 * (below) derives its fixed int_time/again from these same
		 * ceilings, not from the 15 fps/OV5647_AGC_GAIN-range
		 * fallback above -- gating this block on `enable` left
		 * manual mode always using that fallback ceiling regardless
		 * of the sensor's actual configured frame rate.
		 *
		 * Sensor-agnostic exposure ceiling: the sensor's
		 * OWN reported frame period (video_get_frmival(),
		 * forwarded down the isp -> cam -> csi -> sensor
		 * chain by alif_cam_get_frmival()/csi2_dw_get_frmival())
		 * minus a generic 2% blanking margin -- no per-sensor
		 * line-time constant needed. Falls back to the 15 fps
		 * OV5647 constant above if the query fails (no sensor
		 * bound, TPG mode, driver doesn't implement it).
		 */
		struct video_frmival frmival;

		if (video_get_frmival(config->controller, &frmival) == 0 &&
		    frmival.denominator > 0) {
			uint64_t frame_period_us =
				(uint64_t)frmival.numerator * 1000000ULL /
				frmival.denominator;
			uint64_t margin_us = frame_period_us / 50; /* 2% */

			int_time_max_us = (uint32_t)(frame_period_us > margin_us
							      ? frame_period_us - margin_us
							      : frame_period_us);
		}

		/*
		 * Sensor-agnostic gain CEILING only: query the
		 * sensor's OWN registered VIDEO_CID_ANALOGUE_GAIN
		 * range instead of assuming OV5647_AGC_GAIN_MAX -- the
		 * register-to-library-units conversion factor is
		 * still sensor-specific (ISP_AE_GAIN_REG_PER_1X, block
		 * comment above); the range ENDPOINT (max) is not.
		 * again_min is NOT derived from cq.range.min (run 81:
		 * that range's own minimum is the register's absolute
		 * floor, e.g. 0, not a meaningful "1x" reference) --
		 * always the literal 1024 = 1x floor (register
		 * 1024 * ISP_AE_GAIN_REG_PER_1X / 1024 = 16 = 0x10),
		 * declared above, unconditionally: AE should never be
		 * told to command sub-1x gain.
		 */
		struct video_ctrl_query cq = {
			.dev = config->controller,
			.id  = VIDEO_CID_ANALOGUE_GAIN,
		};

		if (video_query_ctrl(&cq) == 0 && cq.range.max > 0) {
			again_max = (uint32_t)cq.range.max * 1024 /
				    ISP_AE_GAIN_REG_PER_1X;
		}
	}

	params.valid_mask   = ISP_PARAM_MASK_AE;
	params.ae.op_mode        = enable ? ISP_OP_AUTO : ISP_OP_MANUAL;
	params.ae.ae_target      = CONFIG_VIDEO_ISP_VSI_AE_TARGET;
	params.ae.damp_over      = 0x40;
	params.ae.damp_under     = 0x40;
	params.ae.tolerance      = 1;
	params.ae.run_interval   = 1;
	params.ae.ae_mode        = ISP_AE_MODE_FIX_FRAME_RATE;
	params.ae.int_time_min   = 32;
	params.ae.int_time_max   = int_time_max_us;
	params.ae.again_min      = again_min;
	params.ae.again_max      = again_max;
	/*
	 * dgain is documented "1x = 256" (isp_ctrl_params.h), but the
	 * write-back's totalGain = aGain * dGain / ISP_SNS_GAIN_ACCU
	 * (ISP_SNS_GAIN_ACCU = 1024, isp_api_wrapper.c) and Alif's own
	 * isp_param_conf.h reference AE block both treat dgain as 1x = 1024
	 * -- confirmed on the bench (run 78): with dgain pinned to 256 (the
	 * documented-but-wrong value) the sensor received exactly 1/4 of the
	 * library's intended gain (register 0x04 instead of 0x10 for a
	 * nominal 1x). Pinned to 1024 (unity in the SAME scale as again) so
	 * all headroom flows through again, matching OV5647's single
	 * combined AGC_GAIN register; the header's "1x = 256" comment
	 * appears to be copied from ISP_WB_GAIN_S's (correct, different)
	 * scale, not verified against this path.
	 */
	params.ae.dgain_min      = 1024;
	params.ae.dgain_max      = 1024;

	if (!enable) {
		/* Manual mode: sets a fixed, non-zero exposure/gain instead
		 * of leaving the library at int_time/again/dgain=0 --
		 * int_time_max/2 (half of the same sensor-derived ceiling
		 * enable=true would use, not the 66514us fallback unless the
		 * queries above failed) at the 1x gain floor.
		 */
		params.ae.int_time = params.ae.int_time_max / 2;
		params.ae.again    = again_min;
		params.ae.dgain    = 1024;
	}

	k_mutex_lock(&data->lib_lock, K_FOREVER);
	int ret = isp_vsi_set_param(&data->init_cfg, &params);

	k_mutex_unlock(&data->lib_lock);

	if (ret) {
		LOG_ERR("Failed to %s AE: %d", enable ? "enable" : "disable", ret);
	}

	return ret;
}

/*
 * Run 84: root cause for AE/AWB never stabilising even after every fix so
 * far (idempotent-skip removed, apply-after-Enable* ordering, again_min/max
 * derivation, maxDgain unity fix -- all real, all necessary, none
 * sufficient). isp_vsi_update_cfg()'s SetCalib() (isp_api_wrapper.c:578)
 * was reloaded -- before patch 0007's once-flag guard -- the ENTIRE
 * isp_param_conf.h calib blob on every restart, including
 * .aem.blockWin (default {0,0,1920,1080}, isp_param_conf.h:54-63) and
 * .wbm.measRect -- both sized for a 1080p sensor, not this camera's 640x480
 * frame.
 *
 * Run 85 correction: h_size/v_size are the WHOLE window, not a per-block
 * size -- the vendor header's "Size of One Block" wording (mpi_isp_expm.h/
 * mpi_isp_wbm.h) is misleading; the library divides the window by its own
 * fixed 5x5 grid internally. Run 84's width/5,height/5 (128/96 in) landed in
 * the ISP_EXP_H_SIZE/V_SIZE hardware registers as 24/18 -- 128/96 divided by
 * 5 AGAIN, i.e. a 5x5-block-of-blocks, not the intended 128x96 window. Bench
 * evidence (run 85, meanLum now 16000 not 0): the grid completed a real
 * measurement even at the wrong window size, just the wrong one -- passing
 * the FULL frame (h_size=width, v_size=height, offsets 0) is what the
 * hardware register readback needs to land on 128/96 (640/5, 480/5).
 *
 * Unlike isp_apply_wb()/isp_apply_ae() (3A convergence STATE -- must NOT be
 * re-applied every restart, run 81's regression), the AEM/WBM windows carry
 * no convergence state of their own -- re-issuing identical values every
 * restart is a no-op once SetCalib itself only runs once (see the run-85
 * hal_alif patch guarding VSI_MPI_ISP_SetCalib with a once-flag,
 * isp_api_wrapper.c). Left unconditional here anyway: cheap, and it also
 * covers a future format change mid-run this driver doesn't currently
 * support switching without a full re-init.
 *
 * wbm.wpRange: max_y/max_c_sum/min_c are YCbCr-mode-only fields, unused in
 * RGB mode (mpi_isp_wbm.h's ISP_WBM_WP_RANGE_S doc) -- left as read back.
 * ref_cr_max_r/min_y_max_g/ref_cb_max_b ARE the RGB-mode upper bounds
 * (mpi_isp_wbm.h:84-96: "RGB Mode: only pixels values R/G/B < MaxR/G/B
 * contribute") -- run 85 bench evidence (WBM count 0, means 0xFF) showed the
 * compiled-in calib's bounds exclude every real (non-clipped-white) pixel in
 * this scene; 0xF0 admits anything short of sensor clipping.
 */
static int isp_apply_aem_wbm(const struct device *dev, uint16_t width, uint16_t height)
{
	struct isp_data *data = dev->data;
	struct isp_params params = {0};
	int ret;

	if (!IS_ENABLED(CONFIG_ISP_LIB_EXPOSUREM_MODULE) &&
	    !IS_ENABLED(CONFIG_ISP_LIB_WBM_MODULE)) {
		return 0;
	}

	k_mutex_lock(&data->lib_lock, K_FOREVER);

	/* Same valid_mask-before-get bug as isp_apply_wb() (see its comment)
	 * -- must be set BEFORE isp_vsi_get_param(), not just before the
	 * matching set_param(), or the wrapper's GET gate
	 * (`params->valid_mask & ISP_PARAM_MASK_*`) drops the read silently.
	 * AEM's own fields are all explicitly overwritten below regardless,
	 * so this didn't poison AEM; WBM's wpRange fields NOT explicitly
	 * overwritten (max_y/max_c_sum/min_c, unused in RGB mode but still
	 * meant to be real read-back values, not zero) WERE silently zeroed.
	 */
	if (IS_ENABLED(CONFIG_ISP_LIB_EXPOSUREM_MODULE)) {
		params.valid_mask |= ISP_PARAM_MASK_AEM;
	}
	if (IS_ENABLED(CONFIG_ISP_LIB_WBM_MODULE)) {
		params.valid_mask |= ISP_PARAM_MASK_WBM;
	}

	ret = isp_vsi_get_param(&data->init_cfg, &params);
	if (ret) {
		k_mutex_unlock(&data->lib_lock);
		LOG_ERR("Failed to read back AEM/WBM calib state: %d", ret);
		return ret;
	}

	if (IS_ENABLED(CONFIG_ISP_LIB_EXPOSUREM_MODULE)) {
		params.aem.enable   = 1;
		params.aem.alt_mode = 0;
		params.aem.h_offs   = 0;
		params.aem.v_offs   = 0;
		params.aem.h_size   = width;
		params.aem.v_size   = height;
	}

	if (IS_ENABLED(CONFIG_ISP_LIB_WBM_MODULE)) {
		/* valid_mask already has ISP_PARAM_MASK_WBM, set before the
		 * get above. */
		params.wbm.enable      = 1;
		params.wbm.meas_mode   = ISP_WBM_MODE_RGB;
		params.wbm.h_offs      = 0;
		params.wbm.v_offs      = 0;
		params.wbm.h_size      = width;
		params.wbm.v_size      = height;
		/* RGB-mode upper bounds -- see the function comment. maxY/
		 * maxCSum/minC untouched (YCbCr-mode-only, left as read back).
		 */
		params.wbm.ref_cr_max_r = 0xF0;
		params.wbm.min_y_max_g  = 0xF0;
		params.wbm.ref_cb_max_b = 0xF0;
	}

	ret = isp_vsi_set_param(&data->init_cfg, &params);

	k_mutex_unlock(&data->lib_lock);

	if (ret) {
		LOG_ERR("Failed to set AEM/WBM measurement windows: %d", ret);
	}

	return ret;
}

/*
 * Runs 153, 156 (patch 0009: zephyr/patches/hal_alif/
 * 0009-isp-setcalib-before-3a-callbacks.patch): the calibration's own
 * OP_TYPE_AUTO AE+AWB converge from the FIRST stream with no help from this
 * driver, so a ctrl left at its calibration-matching default (range.def,
 * isp_init_controls()) needs no isp_vsi_set_param() call, ever -- just
 * record whether the app pushed the value away from that default.
 * isp_stream_start() (the only place still stopped when a change could
 * safely take effect) is what actually calls isp_apply_wb()/isp_apply_ae(),
 * once, the next time it runs; see the ponytail comment below.
 */
static int isp_set_ctrl(const struct device *dev, uint32_t cid)
{
	struct isp_data *data = dev->data;

	switch (cid) {
	/* Any change is re-applied, including a return to the default: once
	 * a SET has moved the lib off its calibration state, only another SET
	 * moves it back.
	 */
	case VIDEO_CID_AUTO_WHITE_BALANCE:
		data->ctrls.wb_dirty = true;
		break;
	case VIDEO_CID_EXPOSURE_AUTO:
		data->ctrls.ae_dirty = true;
		break;
	default:
		return -ENOTSUP;
	}

	/*
	 * ponytail: a ctrl change made while the ISP is already streaming is
	 * only stored here -- it takes effect at the NEXT isp_stream_start()
	 * (bench-observed, runs 131-138: a SET issued mid-stream is stored by
	 * the library but not ACTIVATED until a full stop+restart). Upgrade
	 * path, if a same-stream live toggle turns out to matter: drive a
	 * stop+restart from here instead.
	 */
	return 0;
}

static int isp_init_controls(const struct device *dev)
{
	struct isp_data *data = dev->data;
	int ret;

	if (IS_ENABLED(CONFIG_ISP_LIB_WB_MODULE)) {
		/* Default ON, matching the calibration's own OP_TYPE_AUTO AWB
		 * (patch 0009, runs 153/156) -- not a driver-invented default.
		 */
		ret = video_init_ctrl(&data->ctrls.awb, dev, VIDEO_CID_AUTO_WHITE_BALANCE,
				      (struct video_ctrl_range){.min = 0, .max = 1, .step = 1,
								 .def = 1});
		if (ret) {
			return ret;
		}
	}

	if (IS_ENABLED(CONFIG_ISP_LIB_AE_MODULE)) {
		/* Default AUTO, matching the calibration's own OP_TYPE_AUTO AE
		 * (patch 0009, runs 153/156) -- not a driver-invented default.
		 */
		ret = video_init_menu_ctrl(&data->ctrls.exposure_auto, dev,
					   VIDEO_CID_EXPOSURE_AUTO, VIDEO_EXPOSURE_AUTO, NULL);
		if (ret) {
			return ret;
		}
		/* The calibration carries no AE target, integration-time or
		 * gain range for this sensor: isp_apply_ae() derives them from
		 * CONFIG_VIDEO_ISP_VSI_AE_TARGET and the sensor's own ctrls, so
		 * the first stream start must push them even at the default.
		 * Without it AE drives the OV5647 past its exposure limit and
		 * saturates the frame (bench run 157).
		 */
		data->ctrls.ae_dirty = true;
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
			/* video_bits_per_pixel() returns 0 for this private
			 * fourcc -- alp_isp_default_pitch() (isp_frame_size.h)
			 * knows it (24 bpp, 3 equal 8-bit planes). */
			channel->output_fmt.pitch = alp_isp_default_pitch(tmp_fmt, channel->output_fmt.width);
		}

		*fmt = channel->output_fmt;
		break;
	default:
		LOG_ERR("Unsupported buffer type!");
		return -ENOTSUP;
	}
	return 0;
}

/*
 * ISP_ACQ_PROP's pin mapping selects the ACQ hardware's 8/10/12-bit Bayer
 * decode path.  pix_fmt_bpp() (video_alif.c) reports the fourcc's STORAGE
 * width, not the sensor SAMPLE depth -- for the *unpacked* Bayer formats
 * (e.g. VIDEO_PIX_FMT_SBGGR10, one 10-bit sample stored per 16-bit word)
 * video_bits_per_pixel() returns 16, which fell through isp_stream_start()'s
 * switch below to the 12-bit default and mis-mapped every unpacked 10-bit
 * format.  Key on the true sample depth instead, derived directly from the
 * fourcc's Bayer variant (packed or not).
 *
 * VIDEO_PIX_FMT_Y10P / VIDEO_PIX_FMT_GREY (run 70, bench-confirmed): these
 * generic mono/packed fourccs are ALSO valid ISP INPUT formats
 * (supported_input_fmts[], below) -- isp_pico.c's own isp_set_fmt(INPUT)
 * stores whatever fourcc the caller requests verbatim into
 * port->port_fmt.pixelformat, and the OV5647 real-sensor path (stage 3,
 * examples/aen/aen-isp-ov5647-capture) requests exactly VIDEO_PIX_FMT_Y10P
 * (mirroring Alif's own sdk-alif viewfinder recipe).  Neither fourcc
 * matched any case here, so bayer_sample_depth() silently fell through to
 * the 12-bit default for every real-sensor capture -- confirmed on silicon
 * by reading ISP_ACQ_PROP back after a live capture: PIN_MAPPING (bits
 * [19:17]) read 0 (12-bit) instead of the correct 1 (10-bit) for Y10P/
 * RAW10 data.  The CPI side is independently confirmed correct for the
 * same capture (CAM_CFG's DATA_MASK field read back CPI_DATA_MASK_10_BIT,
 * dynamically set from the negotiated CSI2_DT_RAW10 by
 * alif_cam_set_csi()'s data_mode_settings[] lookup, video_alif.c:530-548)
 * -- so this was an ISP-side-only mis-mapping, not a CPI misconfiguration.
 */
static unsigned int bayer_sample_depth(uint32_t fourcc)
{
	switch (fourcc) {
	case VIDEO_PIX_FMT_SBGGR8:
	case VIDEO_PIX_FMT_SGBRG8:
	case VIDEO_PIX_FMT_SGRBG8:
	case VIDEO_PIX_FMT_SRGGB8:
	case VIDEO_PIX_FMT_GREY:
		return 8;
	case VIDEO_PIX_FMT_SBGGR10:
	case VIDEO_PIX_FMT_SGBRG10:
	case VIDEO_PIX_FMT_SGRBG10:
	case VIDEO_PIX_FMT_SRGGB10:
	case VIDEO_PIX_FMT_SBGGR10P:
	case VIDEO_PIX_FMT_SGBRG10P:
	case VIDEO_PIX_FMT_SGRBG10P:
	case VIDEO_PIX_FMT_SRGGB10P:
	case VIDEO_PIX_FMT_Y10P:
		return 10;
	case VIDEO_PIX_FMT_SBGGR12:
	case VIDEO_PIX_FMT_SGBRG12:
	case VIDEO_PIX_FMT_SGRBG12:
	case VIDEO_PIX_FMT_SRGGB12:
	case VIDEO_PIX_FMT_SBGGR12P:
	case VIDEO_PIX_FMT_SGBRG12P:
	case VIDEO_PIX_FMT_SGRBG12P:
	case VIDEO_PIX_FMT_SRGGB12P:
	default:
		return 12;
	}
}

/*
 * PLANNED FIX (not done here, run 66): 10-bit RAW MI output.
 *
 * supported_output_fmts[] (above) currently has no 10-bit raw entry because
 * isp_pixelfmt_from_fourcc() (isp_api_wrapper.c) has no fourcc mapping to
 * PIXEL_FORMAT_RAW10 (vsi_comm_video.h: PIXEL_FORMAT_RAW10 = 21, distinct
 * from the Bayer PIXEL_FORMAT_BGGR10 = 4 family SetChnAttr rejects for
 * output) -- the closest existing case, VIDEO_PIX_FMT_Y10P, maps to
 * PIXEL_FORMAT_GRBG10 instead, a TODO placeholder the wrapper's own comment
 * flags ("This should have been PIXEL_FORMAT_RAW10, but lib does not
 * support it").  Stage 2 of this bring-up (a byte-identical CPI-vs-ISP A/B)
 * needs raw passthrough, so this gap has to close before then.
 *
 * The fix is a hal_alif patch, NOT an alp-sdk change: add a
 * `case VIDEO_PIX_FMT_Y10P: return PIXEL_FORMAT_RAW10;` (replacing the
 * GRBG10 placeholder) to isp_pixelfmt_from_fourcc() in
 * isp_wrapper/src/isp_api_wrapper.c, then add VIDEO_PIX_FMT_Y10P (or a raw
 * 10-bit fourcc of our own choosing) to supported_output_fmts[] above.
 * alp-sdk already has a carry-patch mechanism for exactly this kind of
 * single-file hal_alif fix: zephyr/patches/hal_alif/NNNN-name.patch, tracked in
 * zephyr/patches.yml (path/sha256sum/module/author/email/date/upstreamable/
 * comments per entry, applied via `west patch apply`, checked by
 * scripts/verify_west_patches.py) -- see
 * zephyr/patches/hal_alif/0001-se-service-add-boot-cpu.patch for the shape
 * of an existing hal_alif entry.  Do NOT hand-edit the vendored
 * isp_api_wrapper.c in the west workspace; carry the fix as a new
 * zephyr/patches/hal_alif/000N-*.patch + patches.yml entry instead, so it
 * survives a `west update` and stays submittable upstream.
 */

/*
 * The ISP core always produces 4:2:2 chroma internally; for a 4:2:0 MI
 * output (YUV420/NV12/NV21) the main resizer must downscale chroma
 * vertically 2:1, or the MI writes a full-height chroma plane into a
 * half-height buffer and wraps ("Main picture Cb/Cr address wrap"),
 * losing/misplacing colour on real scenes. Neither the closed VSI lib nor
 * this wrapper ever programs MRSZ (bench runs 186/187) -- do it directly.
 */
static void isp_apply_mrsz(const struct device *dev, uint32_t pixelformat, uint16_t out_height)
{
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	bool      is_420 = pixelformat == VIDEO_PIX_FMT_YUV420 || pixelformat == VIDEO_PIX_FMT_NV12 ||
	                   pixelformat == VIDEO_PIX_FMT_NV21;

	/*
	 * Defensive: (in_h - 1) divides SCALE_VC below, so a height < 4 must
	 * bypass here instead of a DIV_0_TRP UsageFault. Only heights below 4
	 * are bypassed; odd 4:2:0 heights are rejected earlier by
	 * isp_set_fmt(), which is the real gate -- this is belt-and-braces.
	 */
	if (!is_420 || out_height < 4) {
		/*
		 * Bypass, and clear any 4:2:0 config a previous stream left
		 * armed -- FORMAT_CONV_CTRL takes effect immediately (it is
		 * not shadowed by CFG_UPD), so a stale 0x4 (4:2:0) from a
		 * prior stream must be cleared here too, not just SCALE_VC/
		 * PHASE_VC.
		 */
		sys_write32(0, regs + ISP_MRSZ_FORMAT_CONV_CTRL);
		sys_write32(0, regs + ISP_MRSZ_SCALE_VC);
		sys_write32(0, regs + ISP_MRSZ_PHASE_VC);
		sys_write32(MRSZ_CTRL_CFG_UPD, regs + ISP_MRSZ_CTRL);
		return;
	}

	uint32_t in_h = out_height;
	uint32_t out_h = out_height / 2;
	uint32_t scale_vc = ((out_h - 1) * 65536U) / (in_h - 1);

	sys_write32(scale_vc, regs + ISP_MRSZ_SCALE_VC);
	sys_write32(0, regs + ISP_MRSZ_PHASE_VC);
	sys_write32(MRSZ_FORMAT_CONV_CTRL_FORMAT_420, regs + ISP_MRSZ_FORMAT_CONV_CTRL);
	sys_write32(MRSZ_CTRL_SCALE_VC_ENABLE | MRSZ_CTRL_CFG_UPD | MRSZ_CTRL_AUTO_UPD,
		    regs + ISP_MRSZ_CTRL);
}

static int isp_stream_start(const struct device *dev)
{
	const struct isp_config *config = dev->config;
	uintptr_t regs = DEVICE_MMIO_GET(dev);
	struct isp_data *data = dev->data;
	struct video_buffer *vbuf;
	struct video_buffer vbuf2;

	struct channel_parameters *channel = &data->init_cfg.channel;
	struct port_parameters *port = &data->init_cfg.port;
	uint32_t tmp;
	int ret;
	int err = 0; /* the ORIGINAL failure, preserved across cleanup below */
	struct k_work_sync sync;

	if (data->is_streaming) {
		LOG_DBG("Already streaming");
		return -EBUSY;
	}

	/*
	 * Cancel any stale work from a previous session before starting --
	 * this either cancelled a pending bottom half (dropping a
	 * frame-end) or blocked on a running one, so no isp_bottom_half()
	 * invocation from before this start can still be in flight once it
	 * returns. Bench T4 fix: moved AFTER the is_streaming check above --
	 * a no-op restart call (already streaming, about to -EBUSY out) used
	 * to cancel-sync cb_work anyway, tearing down the running bottom
	 * half of a stream this call isn't actually going to (re)start.
	 */
	k_work_cancel_sync(&data->cb_work, &sync);

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
		LOG_ERR("Failed to update ISP config to input/output formats and ROI! "
			"isp_vsi_update_cfg=%d",
			ret);
		data->curr_vid_buf = 0;
		return ret;
	}

	/* Run 84: unconditional, every restart -- see isp_apply_aem_wbm()'s
	 * own comment for why this does NOT wait for isp_vsi_start() or gate
	 * on wb_dirty/ae_dirty the way isp_apply_wb()/isp_apply_ae() do.
	 * Logged, not fatal: a stale (but non-zero) measurement window is
	 * still better than aborting the whole stream start over it.
	 */
	ret = isp_apply_aem_wbm(dev, port->port_fmt.width, port->port_fmt.height);
	if (ret) {
		LOG_WRN("AEM/WBM measurement window apply failed: %d (continuing)", ret);
	}

	tmp = sys_read32(regs + ISP_ACQ_PROP);
	tmp &= ~(ACQ_PROP_PIN_MAPPING_MASK << ACQ_PROP_PIN_MAPPING_SHIFT);

	switch (bayer_sample_depth(port->port_fmt.pixelformat)) {
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
		LOG_ERR("Failed to assign buffer to hardware! isp_vsi_enqueue=%d", ret);
		data->curr_vid_buf = 0;
		return ret;
	}

	/* Run 90-107 bisect fix: gate the SENSOR (EXPOSURE_AUTO/AUTOGAIN) to
	 * MANUAL before the ISP starts capturing -- see
	 * isp_apply_ae_sensor_gate()'s own comment for why this can no
	 * longer be deferred alongside the library-side AE param push, and
	 * why it is unconditional regardless of which AE mode the library
	 * itself is in. */
	isp_apply_ae_sensor_gate(dev);

	/* Set is_streaming BEFORE starting hardware to prevent
	 * bottom_half from stopping CPI mid-start
	 */
	data->is_streaming = true;

	ret = isp_vsi_start(&data->init_cfg);
	if (ret) {
		LOG_ERR("Failed to start stream! isp_vsi_start=%d", ret);
		data->is_streaming = false;
		err = ret;
		goto dequeue_buf;
	}

	/*
	 * Placement: after isp_vsi_start() (above), before the camera path's
	 * video_stream_start(config->controller) (below). FORMAT_CONV_CTRL is
	 * not CFG_UPD-shadowed, so this write takes effect the instant it
	 * lands -- safe here on the camera path because no pixel data flows
	 * until video_stream_start(config->controller), which runs after
	 * this call. Do not move this below video_stream_start().
	 *
	 * In TPG mode (config->controller == NULL) the TPG is the ISP's own
	 * internal pattern source, so it may already be producing frames
	 * right after isp_vsi_start() returns -- this write can then land
	 * mid-frame. TPG colour output is tracked separately (#2256); no fix
	 * here.
	 */
	isp_apply_mrsz(dev, channel->output_fmt.pixelformat, channel->output_fmt.height);

	/*
	 * Runs for any ctrl the app changed (isp_set_ctrl()), plus AE once at
	 * the first start (isp_init_controls(): the calibration has no AE
	 * limits for this sensor). Default AWB needs no call at all -- the
	 * calibration already runs it (patch 0009, runs 153/156). Right
	 * here, synchronously, after isp_vsi_start()'s Enable* above, is the
	 * one proven-working order (run 74): the AWB/AE callbacks aren't live
	 * until Enable* runs, so an apply issued before it would just be
	 * dropped. SetCalib (isp_vsi_update_cfg(), above) itself now runs at
	 * most twice per boot -- once at init (patch 0009), once at the
	 * first isp_vsi_update_cfg() (patch 0007's once-guard) -- never on a
	 * later restart, so this apply doesn't need to fight a reload; it
	 * just needs Enable* to have already happened. isp_stream_start()
	 * only ever runs while stopped
	 * (guarded at the top of this function), so this is also exactly
	 * "the next stream start while the ISP is stopped" a mid-stream ctrl
	 * change waits for (see isp_set_ctrl()'s ponytail comment).
	 */
	if (data->ctrls.wb_dirty) {
		bool wb_enable = (data->ctrls.awb.val != 0);
		int ret_wb = isp_apply_wb(dev, wb_enable);

		if (ret_wb == 0) {
			data->ctrls.wb_dirty = false;
		}
	}
	if (data->ctrls.ae_dirty) {
		bool ae_enable = (data->ctrls.exposure_auto.val == VIDEO_EXPOSURE_AUTO);
		int ret_ae = isp_apply_ae(dev, ae_enable);

		if (ret_ae == 0) {
			data->ctrls.ae_dirty = false;
		}
	}

	/*
	 * In TPG mode (no camera controller wired -- config->controller is a
	 * legitimate NULL here, see video_isp_init()'s init-time check) there is
	 * no endpoint device to forward stream-start to: the TPG is the ISP's
	 * OWN internal pattern source.  video_stream_start(NULL, ...) returns
	 * -EINVAL via its own dev==NULL guard (zephyr/include/zephyr/drivers/
	 * video.h) WITHOUT touching *dev -- so calling it unconditionally does
	 * not crash, but it always "fails" and previously masked a real
	 * config->controller->name NULL dereference in the LOG_ERR below it.
	 * v4.4 video-API shim (Alp Lab AB): video_stream_start gained an
	 * `enum video_buf_type`; the controller is the capture source ->
	 * VIDEO_BUF_TYPE_OUTPUT.
	 */
	if (config->controller) {
		ret = video_stream_start(config->controller, VIDEO_BUF_TYPE_OUTPUT);
		if (ret) {
			LOG_ERR("Failed to start stream for Endpoint device: %s! "
				"video_stream_start=%d",
				config->controller->name,
				ret);
			data->is_streaming = false;
			err = ret;
			goto stop_isp_stream;
		}
	}

	return 0;

stop_isp_stream:
	ret = isp_vsi_stop(&data->init_cfg);
	if (ret) {
		/* Cleanup failure: log it, but do NOT let it overwrite `err` --
		 * the caller needs the ORIGINAL failure (set above), not
		 * whichever of these two cleanup calls happened to also fail.
		 */
		LOG_ERR("Failed to stop ISP device streaming (cleanup): isp_vsi_stop=%d", ret);
	}
dequeue_buf:
	ret = isp_vsi_dequeue(&data->init_cfg, &vbuf2);
	if (ret) {
		LOG_ERR("Failed to dequeue buffer back (cleanup): isp_vsi_dequeue=%d", ret);
	}

	return err;
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

	/*
	 * In TPG mode config->controller is a legitimate NULL (see
	 * video_isp_init()'s init-time check) -- there is no endpoint device to
	 * forward stream-stop to.  Calling video_stream_stop(NULL, ...)
	 * unconditionally previously made this function ALWAYS return -EINVAL
	 * in TPG mode (its own dev==NULL guard) before ever reaching
	 * isp_vsi_stop() below, so the ISP hardware never actually stopped.
	 * v4.4 video-API shim (Alp Lab AB): video_stream_stop gained an
	 * `enum video_buf_type`; the controller is the capture source ->
	 * VIDEO_BUF_TYPE_OUTPUT.
	 */
	if (config->controller) {
		ret = video_stream_stop(config->controller, VIDEO_BUF_TYPE_OUTPUT);
		if (ret) {
			LOG_ERR("Failed to stop streaming in pipeline! video_stream_stop=%d",
				ret);
			return ret;
		}
	}

	ret = isp_vsi_stop(&data->init_cfg);
	if (ret) {
		LOG_ERR("Failed to stop ISP from streaming! isp_vsi_stop=%d", ret);
		return ret;
	}

	data->curr_vid_buf = 0;
	data->is_streaming = false;

	return 0;
}

/* v4.4 video-API shim (Alp Lab AB): set_stream gained an `enum video_buf_type
 * type` param (unused here -- this m2m device streams a single pipeline).
 *
 * Alif's own model (kept): the driver auto-stops on its own once the IN-FIFO
 * runs dry (isp_bottom_half()'s "No more empty buffers" branch) and only
 * restarts the next time a caller calls video_stream_start() again --
 * src/backends/camera/alif_isp_pico.c's isp_capture() re-issues it before
 * every dequeue, examples/aen/aen-isp-ov5647-capture's per-frame loop does
 * the same. No driver-driven restart of its own.
 */
static int isp_set_stream(const struct device *dev, bool enable, enum video_buf_type type)
{
	ARG_UNUSED(type);

	return enable ? isp_stream_start(dev) : isp_stream_stop(dev);
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
 *
 * Zephyr v4.4's video_stream_stop() helper (drivers/video.h) is
 * set_stream(dev, false) THEN video_flush(dev, true), two SEPARATE
 * top-level calls from the same caller thread -- so a user stop reaches
 * this function moments after isp_set_stream() already returned.
 */
static int isp_flush(const struct device *dev, bool cancel)
{
	const struct isp_config *config = dev->config;
	struct isp_data *data = dev->data;

	uintptr_t regs = DEVICE_MMIO_GET(dev);
	struct video_buffer *vbuf = NULL;
	struct k_work_sync sync;

	int ret;

	if (cancel) {
		/* Case when video stream processing needs to be stopped. */
		hw_disable_mi_interrupts(regs, MI_INTR_MP_FRAME_END);

		/*
		 * Cancel-sync the bottom half BEFORE the drain below hands
		 * every queued buffer back to fifo_out. isp_bottom_half()
		 * (data->cb_workq) can already be queued -- or mid-run -- at
		 * this point despite the interrupt disable just above (it
		 * only stops NEW frame-end interrupts, not a bottom half
		 * already submitted from one that fired earlier); left
		 * uncancelled it can still attach a buffer via
		 * isp_vsi_enqueue() after this function has handed the
		 * buffers back to the caller. Runs on the caller thread, no
		 * lock held -- matches isp_stream_start()'s own
		 * k_work_cancel_sync() of the same work item.
		 */
		k_work_cancel_sync(&data->cb_work, &sync);

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
	} else if (!data->is_streaming) {
		/*
		 * Not actually running (a starvation stop, or a stream that
		 * was never started) -- nothing will ever drain fifo_in on
		 * its own (no active DMA, no more frame-ends coming), so the
		 * wait below would spin forever. Move every queued buffer
		 * straight to fifo_out instead, the same as the cancel=true
		 * path does above. Keyed on is_streaming, not curr_vid_buf:
		 * is_streaming is the authoritative "is a DMA in flight"
		 * state this driver already uses everywhere else
		 * (isp_bottom_half(), isp_stream_start()/_stop());
		 * curr_vid_buf can lag it by a beat in some paths and isn't
		 * the thing actually being tested here.
		 */
		while ((vbuf = k_fifo_get(&data->fifo_in, K_NO_WAIT))) {
			k_fifo_put(&data->fifo_out, vbuf);
		}
	} else {
		/*
		 * A DMA is in flight: wait for isp_bottom_half() (on
		 * data->cb_workq) to drain fifo_in the normal way, one
		 * frame-end at a time -- it flips is_streaming false once the
		 * IN-FIFO runs dry (this driver's own auto-stop, kept from
		 * Alif's original model). Bounded, not indefinite: a wedged
		 * DMA (a lost frame-end interrupt, a hung bottom half) must
		 * not hang the caller thread forever. Bounded at 500ms total,
		 * not per frame-end -- a deep queue or a low frame rate can
		 * legitimately need more than that to drain and will
		 * correctly hit -ETIMEDOUT below rather than being given an
		 * unbounded wait.
		 */
		for (int i = 0; i < 500 && !k_fifo_is_empty(&data->fifo_in) &&
				data->is_streaming;
		     i++) {
			k_msleep(1);
		}

		if (data->is_streaming && !k_fifo_is_empty(&data->fifo_in)) {
			/*
			 * Timed out with the MI still DMA-ing into
			 * curr_vid_buf: draining fifo_in here would hand the
			 * caller a buffer hardware is actively writing, and
			 * this branch never stops the ISP, so clearing
			 * is_streaming would be a lie about what is actually
			 * running. Leave every queue and all state untouched
			 * and report the timeout -- the caller can retry with
			 * cancel=true, which stops the ISP before it drains.
			 */
			return -ETIMEDOUT;
		}

		/* The wait above ended without timing out -- fifo_in drained
		 * naturally, or is_streaming flipped false (the bottom
		 * half's own auto-stop) with a few buffers still queued.
		 * Hand those back the same way the not-streaming branch
		 * above does.
		 */
		while ((vbuf = k_fifo_get(&data->fifo_in, K_NO_WAIT))) {
			k_fifo_put(&data->fifo_out, vbuf);
		}
	}

	if (!cancel) {
		/*
		 * The cancel=true path above already cancel-synced
		 * data->cb_work; this path never does (it only waits for
		 * fifo_in to empty, either because the bottom half is
		 * legitimately still draining it or because is_streaming was
		 * already false). A bottom half can still be RUNNING here --
		 * it has taken the last buffer off fifo_in (so the wait loop
		 * above saw an empty fifo and exited) but not yet finished
		 * and cleared is_streaming -- so flush it out before this
		 * function overwrites curr_vid_buf/is_streaming itself.
		 * Runs on the caller thread with no lock held, same as the
		 * cancel path's cancel_sync just above.
		 */
		k_work_flush(&data->cb_work, &sync);
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

	/*
	 * Flush BEFORE the buffer is visible on fifo_in, not after: once
	 * k_fifo_put() runs, a concurrent caller-driven video_stream_start()
	 * on another thread, or (if this is the buffer isp_bottom_half() is
	 * about to pick up next) the ISR-driven bottom half itself, can
	 * immediately attach this buffer to hardware and start a DMA into
	 * it. A flush that runs AFTER the buffer is queued can land after
	 * that DMA has already started, writing this core's dirty cache
	 * lines back over frame data the hardware already wrote.
	 */
	(void)sys_cache_data_flush_and_invd_range(buf->buffer, buf->size);

	k_fifo_put(&data->fifo_in, buf);

	LOG_DBG("Enqueued buffer: Addr - 0x%x, size - %d, bytesused - %d",
		(uint32_t)buf->buffer, buf->size, buf->bytesused);

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

	/*
	 * Full frame size, not pitch*height: pitch (isp_set_fmt(), above) is
	 * the LUMA-only line stride for planar/semi-planar YUV, so
	 * pitch*height covers only the Y plane and drops the U/V planes --
	 * bench-proven on E1M-AEN803 (run 200): every dequeued YUV420/NV12
	 * buffer reported bytesused 0 (pitch was 0 before the isp_set_fmt()
	 * fix, above) and callers copied nothing.  alp_isp_frame_size()
	 * (isp_frame_size.h) -- the same helper
	 * src/backends/camera/alif_isp_pico.c sizes its buffer pool with --
	 * reports the format's true average bits/pixel INCLUDING chroma, so
	 * this is correct for every output fourcc this driver's
	 * supported_output_fmts[] advertises.
	 *
	 * Capped at buf->size: alp_isp_frame_size() sizes the FORMAT, not
	 * this particular buffer, so a caller that enqueued something
	 * smaller than the negotiated frame (a pool-sizing bug, or a format
	 * this helper doesn't yet know) would otherwise hand
	 * sys_cache_data_invd_range(), below, a length that invalidates past
	 * the buffer's end. video_buffer_aligned_alloc() rounds every real
	 * allocation UP to CONFIG_VIDEO_BUFFER_POOL_ALIGN, so this cap is a
	 * last-line-of-defense, not the expected path.
	 */
	uint32_t frame_size = alp_isp_frame_size(
	    channel->output_fmt.pixelformat, channel->output_fmt.width, channel->output_fmt.height);

	if (frame_size > (*buf)->size) {
		LOG_WRN("Dequeued buffer (%u B) is smaller than the negotiated frame (%u B); "
		        "bytesused capped, dequeued frame will be truncated",
		        (*buf)->size,
		        frame_size);
	}
	(*buf)->bytesused = MIN(frame_size, (*buf)->size);

	/*
	 * Invalidate what the ISP's MI (memory interface) DMA just wrote.  The
	 * enqueue path already cleans+invalidates the buffer before the transfer
	 * (isp_enqueue(), above), but nothing invalidates it afterwards, so with
	 * CONFIG_DCACHE=y and no nocache placement the application could read
	 * cache lines that speculative prefetch pulled in during the ISP's
	 * capture window -- stale pixels, no error.  Same stale-cache class the
	 * CPI driver fixes at video_alif.c's alif_cam_dequeue() (#1825).
	 */
	(void)sys_cache_data_invd_range((*buf)->buffer, (*buf)->bytesused);

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
 * removes the only callers of isp_vsi_set_param / isp_vsi_get_param -- both
 * ARE exported by the locally vendored hal_alif libisp wrapper
 * (isp_api_wrapper.c:880, :1242; see the HAL_ALIF note at the top of this
 * file); they simply have no caller left in this port until the
 * control-registry wiring lands.  Do NOT fabricate an API that already exists.
 */
static DEVICE_API(video, isp_driver_api) = {
	.set_format = isp_set_fmt,
	.get_format = isp_get_fmt,
	.set_stream = isp_set_stream,
	.get_caps = isp_get_caps,
	.flush = isp_flush,
	.enqueue = isp_enqueue,
	.dequeue = isp_dequeue,
	.set_ctrl = isp_set_ctrl,
#ifdef CONFIG_POLL
	.set_signal = isp_set_signal,
#endif /* CONFIG_POLL */
};

/*
 * alp-sdk localization (Alp Lab AB): the vendored isp-vsi.h declares this as a
 * PLAIN prototype, not a __syscall (see the note there) -- the Zephyr syscall
 * generator does not scan this header in the alp-sdk module context, so the
 * upstream `__syscall` + z_impl_/z_vrfy_ split does not build here.  This is
 * the direct (non-marshalled) implementation; no CONFIG_USERSPACE variant
 * exists for it.
 */
int isp_vsi_register_ae_status_callback(const struct device *dev,
		isp_ae_status_cb ae_status_cb, void *user_data)
{
	struct isp_data *data = dev->data;

	data->init_cfg.ae_status_cb = ae_status_cb;
	data->init_cfg.ae_status_user_data = user_data;

	return 0;
}

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
	 * Setup FIFO for ISP driver.
	 */
	k_fifo_init(&data->fifo_in);
	k_fifo_init(&data->fifo_out);
	data->dev = dev;

	k_mutex_init(&data->lib_lock);

	/*
	 * Setup interrupts -- only now that everything the ISR's bottom half
	 * touches exists. A core reset does not reset the ISP: an MI
	 * frame-end latched by the previous image fires the moment the IRQ
	 * is enabled, and with the IRQs enabled first it ran the bottom half
	 * on an uninitialised lib_lock (HardFault, bench run 165).
	 */
	config->irq_config_func(dev);

	ret = isp_init_controls(dev);
	if (ret) {
		LOG_ERR("Failed to register ISP video controls!");
		return ret;
	}

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
		CONFIG_VIDEO_ISP_VSI_INIT_PRIORITY,                                           \
		&isp_driver_api);                                                             \
		                                                                              \
	/* Chains this device onto v4.4's control-registry walk (video_find_ctrl(),         \
	 * drivers/video/video_ctrls.c): an app calling video_get_ctrl()/                    \
	 * video_set_ctrl() on the ISP device for a control the ISP itself doesn't          \
	 * register (e.g. exposure/AWB, owned by the sensor) falls through to               \
	 * .src_dev and keeps walking upstream.  src_dev mirrors .controller above --       \
	 * NULL in TPG-only configs (no camera port@0 wired), which the framework           \
	 * handles by stopping the walk there. */                                          \
	VIDEO_DEVICE_DEFINE(isp_vdev_##i, DEVICE_DT_INST_GET(i),                            \
			     DEVICE_DT_GET_OR_NULL(REMOTE_DEVICE(i, 0)));                    \
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
