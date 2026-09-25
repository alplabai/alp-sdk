/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Alp Lab AB
 *
 * Alif Ensemble ISP-Pico-aware camera backend.
 *
 * Wraps the same Zephyr drivers/video/ sensor + buffer plumbing the portable
 * zephyr_video backend uses (DT alias alp-camera0..3 + the portable v4.4 video
 * API: video_get_caps / video_set_format / video_enqueue / video_dequeue), but
 * advertises the Alif Ensemble on-die ISP-Pico (Verisilicon ISP Nano) block via
 * the configure_isp op so apps that opt into the higher-priority backend can
 * drive AE / AWB / AF + the picture-tuning offsets through the same vtable.
 *
 * Mirrors src/backends/camera/v2n_n44_isp.c (the V2N N44 ISP backend): same
 * stub-vs-real split, and both now talk the UPSTREAM v4.4 video API --
 * video_get_caps(dev, &caps) with caps.type, video_set_format(dev, &fmt),
 * video_buffer_aligned_alloc(size, align, K_NO_WAIT), video_stream_start(dev, type).  This
 * file was the first to use it, which is why it reads as the reference; the
 * v2n backend and the portable zephyr_video.c were ported to match.
 *
 * Why a separate backend rather than a Kconfig switch on the portable one:
 *   1. The ISP-Pico is an Alif Ensemble m2m video device (vsi,isp-pico, driven
 *      by zephyr/drivers/video/isp_pico.c); the portable backend stays clean of
 *      the vendor ISP device + the hal_alif libisp blob.
 *   2. priority=100 on silicon_ref="alif:ensemble:e8" overrides the
 *      zephyr_video registration (priority 50) on E8 builds while keeping the
 *      portable backend the natural default everywhere else.
 *
 * Stub vs real split:
 *   - The sensor pipeline (open / start / stop / capture / release / close)
 *     routes through Zephyr's v4.4 video API verbatim -- those functions are
 *     NOT stubs.  Pointed at the vsi,isp-pico device (alp-camera0, aliased to
 *     &isp -- not the raw &csi_capture_port -- for the OV5647 ISP path), they
 *     drive the real ISP m2m path the moment the hal_alif libisp wrapper is
 *     bumped to the version isp_pico.c targets.
 *   - open() now drives the exact sequence examples/aen/aen-isp-ov5647-
 *     capture proves on real silicon (runs 69-145): ISP INPUT format
 *     SBGGR10P at the requested size, AWB (VIDEO_CID_AUTO_WHITE_BALANCE) and
 *     AE (VIDEO_CID_EXPOSURE_AUTO) turned on via the standard Zephyr video
 *     ctrl registry before the first video_stream_start(), the caller's
 *     requested frame interval (cfg->fps, falling back to a 10 fps default
 *     -- a choice, not a hardware limit -- when unset; see the DT_NODE_EXISTS
 *     block below), then the ISP OUTPUT format.  The ISP MI
 *     never produces RGB565 (isp_pico.c's supported_output_fmts is
 *     YUV/mono/Bayer only) -- a caller requesting ALP_PIXFMT_RGB565 gets a
 *     negotiated YUV output (YUV420 planar preferred, YUYV fallback -- see
 *     _rgb565_yuv_candidates[] below: YUYV frames never completed on
 *     silicon in run 154, and YUV420 matches both Alif's own viewfinder
 *     sample and the bench-proven aen-isp-ov5647-capture example)
 *     converted to RGB565 on the CPU by isp_capture(); a caller requesting
 *     a YUV format the ISP produces natively (ALP_PIXFMT_YUV420_PLANAR /
 *     ALP_PIXFMT_NV12) gets it passed through unmodified.
 *   - configure_isp() validates the input, latches the config into backend
 *     state, and returns ALP_OK -- the AWB/AE ctrls open() now drives ARE
 *     the "matching libisp parameter upload" the previous version of this
 *     comment described as TBD; configure_isp() itself stays a latch-only
 *     op (ADR: prefer the standard video ctrl registry over reimplementing
 *     3A tuning in this backend -- see isp_pico.c's isp_set_ctrl()).
 *
 * ADR 0017 Tier-2, OPT-IN (CONFIG_ALP_SDK_CAMERA_ALIF_ISP, default n).
 * vendor-ext. Runtime capture proven on isp_pico.c's own bench app
 * (examples/aen/aen-isp-ov5647-capture); this PORTABLE backend itself is
 * still bench-unverified end to end.
 *
 * AE-convergence fix (bench run 147, examples/aen/aen-isp-ov5647-viewfinder):
 * the naive alp_yuv_to_rgb565() reference converter costs on the order of a
 * second per 640x480 frame at this bench's -Os + CONFIG_DCACHE=n config
 * (scripts/bench/aen/aen-bench-shared.conf) -- far longer than one frame
 * period at the 10 fps this backend requested by default at the time (a
 * choice made for AE headroom in a dim scene, not a sensor limit -- see the
 * open()-time frmival request below). With only
 * CONFIG_ALP_SDK_CAMERA_ALIF_ISP_VBUF_COUNT=2 raw buffers, holding one
 * buffer that long during conversion left just one buffer queued to the
 * ISP MI, which starved, auto-stopped, and forced a full stream restart
 * every frame -- so AE never saw two consecutive frames and stayed pinned
 * at its initial exposure (all-zero RGB565 from frame 5 on). The fix has
 * two parts, both in this file: (1) VBUF_COUNT is now 3, so two buffers
 * stay queued (not one) while the third converts; (2) isp_capture() calls
 * the table-driven fast path in yuv_to_rgb565.h
 * (alp_yuyv_frame_to_rgb565() / alp_yuv420_frame_to_rgb565()), bit-exact
 * with the reference but with the per-pixel multiplies hoisted into
 * lookup tables, instead of alp_yuv_to_rgb565() per pixel.
 *
 * @par Tracking: github.com/alplabai/alp-sdk/issues/223
 */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/video.h>
#include <zephyr/drivers/video-controls.h>
#include <zephyr/drivers/video/isp_frame_size.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/camera.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

LOG_MODULE_REGISTER(alp_camera_alif_isp_pico, CONFIG_LOG_DEFAULT_LEVEL);

#include "alp_errno.h"
#include "camera_ops.h"
#include "alif_isp_pico.h"
#include "alp_slot_claim.h"
#include "yuv_to_rgb565.h"

#ifndef CONFIG_ALP_SDK_MAX_CAMERA_HANDLES
#define CONFIG_ALP_SDK_MAX_CAMERA_HANDLES 2
#endif

#define ALP_ALIF_ISP_DEV_OR_NULL(idx) \
	COND_CODE_1(DT_NODE_HAS_STATUS(DT_ALIAS(_CONCAT(alp_camera, idx)), okay), \
	            (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_camera, idx)))), \
	            (NULL))

static const struct device *const _devs[] = {
	ALP_ALIF_ISP_DEV_OR_NULL(0),
	ALP_ALIF_ISP_DEV_OR_NULL(1),
	ALP_ALIF_ISP_DEV_OR_NULL(2),
	ALP_ALIF_ISP_DEV_OR_NULL(3),
};

static alp_alif_isp_pico_state_t _state_pool[CONFIG_ALP_SDK_MAX_CAMERA_HANDLES];

/* issue #1115 round-2 dev review: claim atomically (in_use is the LAST
 * member; memset only the bytes ahead of it -- see alif_isp_pico.h)
 * instead of the previous plain check-then-set. */
static alp_alif_isp_pico_state_t *_alloc_state(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(_state_pool); ++i) {
		if (alp_slot_try_claim(&_state_pool[i].in_use)) {
			memset(&_state_pool[i], 0, offsetof(alp_alif_isp_pico_state_t, in_use));
			return &_state_pool[i];
		}
	}
	return NULL;
}

static void _free_state(alp_alif_isp_pico_state_t *s)
{
	if (s != NULL) {
		alp_slot_release(&s->in_use);
	}
}

static alp_status_t _errno_to_alp(int err)
{
	/* Delegates to the shared negative-errno baseline (issue #1638).
	 * This switch was one of 27 hand-copied copies that had drifted; the
	 * arms it carried all agreed with the baseline, so the mapping it
	 * produced for them is unchanged. */
	return alp_status_from_zephyr_errno(err);
}

/* RGB565 is handled separately (negotiated as YUV + converted, see
 * _rgb565_yuv_candidates below) -- the ISP MI never offers it directly.
 * RGB888 / ARGB8888 aren't in isp_pico.c's supported_output_fmts either
 * (its only RGB-shaped output is RGB888_PLANAR_PRIVATE, a private planar
 * layout, not VIDEO_PIX_FMT_RGB24/XRGB32) and this backend doesn't convert
 * to them, so they stay mapped to fourccs the ISP will never match --
 * open() reports that as ALP_ERR_OUT_OF_RANGE, same as today. */
static uint32_t _to_video_fourcc(alp_pixfmt_t fmt)
{
	switch (fmt) {
	case ALP_PIXFMT_RGB888:
		return VIDEO_PIX_FMT_RGB24;
	case ALP_PIXFMT_ARGB8888:
		return VIDEO_PIX_FMT_XRGB32;
	case ALP_PIXFMT_YUV420_PLANAR:
		return VIDEO_PIX_FMT_YUV420;
	case ALP_PIXFMT_NV12:
		return VIDEO_PIX_FMT_NV12;
	default:
		return 0u;
	}
}

/*
 * ISP INPUT is always our sensor's native Bayer layout. OV5647 (the shield examples/aen/
 * aen-isp-ov5647-capture bench-proved this backend against) is SBGGR10P -- see that file's header
 * for why SBGGR10P and not the GRBG10 the wrapper otherwise falls back to for an unmapped fourcc.
 * IMX296 (issue #2287 Stage B) is SRGGB10P instead -- a different Bayer PHASE, not just a
 * different sensor, see zephyr/drivers/video/imx296.c's own Bayer-order comment on
 * imx296_fmts[] -- so this can't be one fixed fourcc for every board any more.
 *
 * DT_HAS_COMPAT_STATUS_OKAY(sony_imx296), not video_get_format() on the ISP input device: this
 * has to be a COMPILE-TIME choice. Querying the input device's format at runtime instead would
 * call into alif_cam_set_fmt() (video_alif.c) as a SIDE EFFECT of just reading it back -- and
 * that call fails outright at IMX296's 1088-tall full-frame height (unrelated to this fourcc
 * choice), so probing the input device before this backend has even picked a fourcc for it would
 * break this backend on IMX296 before ALIF_ISP_INPUT_FOURCC even mattered. Every board this SDK
 * ships today has exactly one Bayer sensor wired to the ISP, so "the one sony,imx296 node exists"
 * is an unambiguous stand-in for "IMX296 is this board's sensor" -- a board wiring both OV5647 and
 * IMX296 to the same ISP input at once is not a configuration this SDK supports.
 */
#if DT_HAS_COMPAT_STATUS_OKAY(sony_imx296)
#define ALIF_ISP_INPUT_FOURCC VIDEO_PIX_FMT_SRGGB10P
#else
#define ALIF_ISP_INPUT_FOURCC VIDEO_PIX_FMT_SBGGR10P
#endif

/* Output fourccs this backend converts to RGB565 on the CPU when the
 * caller requests ALP_PIXFMT_RGB565, tried in this order: YUV420 planar
 * first -- bench-proven end to end by examples/aen/aen-isp-ov5647-capture
 * (runs 69-145) and matching Alif's own viewfinder sample -- then YUYV as
 * the fallback if a driver build doesn't offer YUV420. YUYV was tried
 * first in an earlier revision (packed 4:2:2, no chroma-plane subsampling
 * to unpack) but its frames never completed on silicon (run 154); demoted
 * to fallback rather than dropped, in case a future driver/format
 * combination needs it. */
static const uint32_t _rgb565_yuv_candidates[] = {
	VIDEO_PIX_FMT_YUV420,
	VIDEO_PIX_FMT_YUYV,
};

/* Try each of candidates[0..n_candidates) as the OUTPUT fourcc, in order,
 * against the driver's advertised caps at width x height; the first match
 * is set on the device and returned via *fmt_out.
 * ponytail: no fallback to video_get_format() when vcaps->format_caps is
 * NULL (the pre-existing "driver publishes no caps" escape hatch) -- every
 * fourcc this backend ever asks isp_pico.c for IS in its published
 * supported_output_fmts, so that branch was dead for this device; add it
 * back if a future ISP driver build stops publishing caps. */
static bool _negotiate_output_fmt(const struct device     *dev,
                                  const struct video_caps *vcaps,
                                  const uint32_t          *candidates,
                                  size_t                   n_candidates,
                                  uint16_t                 width,
                                  uint16_t                 height,
                                  struct video_format     *fmt_out)
{
	if (vcaps->format_caps == NULL) {
		return false;
	}
	for (size_t c = 0; c < n_candidates; ++c) {
		for (const struct video_format_cap *fc = vcaps->format_caps; fc->pixelformat != 0u; ++fc) {
			if (fc->pixelformat != candidates[c] || width < fc->width_min ||
			    width > fc->width_max || height < fc->height_min || height > fc->height_max) {
				continue;
			}
			fmt_out->type        = VIDEO_BUF_TYPE_OUTPUT;
			fmt_out->pixelformat = candidates[c];
			fmt_out->width       = width;
			fmt_out->height      = height;
			fmt_out->pitch       = 0u;
			return video_set_format(dev, fmt_out) == 0;
		}
	}
	return false;
}

/* Release every video_buffer this handle acquired, getting the driver's
 * queue out of the way first.  video_stream_stop() implies a CANCEL flush
 * (video.h: `video_flush(dev, true)` moves everything the driver holds
 * from its incoming queue to the outgoing one as VIDEO_BUF_ABORTED), so a
 * stop + drain-dequeue detaches the buffers from the device before
 * video_buffer_release() returns them to the shared pool.  Releasing a
 * buffer the driver still queues would recycle a pool slot the device can
 * later hand back -- a stale pointer on the next open (#246). */
static void _release_vbufs(alp_alif_isp_pico_state_t *st)
{
	struct video_buffer *vb = NULL;

	(void)video_stream_stop(st->dev, VIDEO_BUF_TYPE_OUTPUT);
	while (video_dequeue(st->dev, &vb, K_NO_WAIT) == 0 && vb != NULL) {
		vb = NULL;
	}
	for (size_t i = 0; i < ARRAY_SIZE(st->vbufs); ++i) {
		if (st->vbufs[i] != NULL) {
			(void)video_buffer_release(st->vbufs[i]);
			st->vbufs[i] = NULL;
		}
	}
	st->vbuf_count = 0;
}

/* ============================================================== */
/* Sensor / capture path -- delegates to Zephyr drivers/video/    */
/* through the portable v4.4 video API.                           */
/* ============================================================== */

static alp_status_t isp_open(const alp_camera_config_t  *cfg,
                             alp_camera_backend_state_t *state,
                             alp_capabilities_t         *caps_out)
{
	if (cfg == NULL || cfg->camera_id >= ARRAY_SIZE(_devs)) {
		return ALP_ERR_INVAL;
	}
	/* The ISP only outputs processed frames.  A raw or mono request must
	 * fail here rather than silently keep whatever output format the ISP
	 * already happens to be in. */
	if (cfg->format == ALP_PIXFMT_GREY8 || cfg->format == ALP_PIXFMT_RAW8 ||
	    cfg->format == ALP_PIXFMT_RAW10) {
		return ALP_ERR_NOSUPPORT;
	}
	const struct device *dev = _devs[cfg->camera_id];
	if (dev == NULL || !device_is_ready(dev)) {
		return ALP_ERR_NOT_READY;
	}

	alp_alif_isp_pico_state_t *st = _alloc_state();
	if (st == NULL) {
		return ALP_ERR_NOMEM;
	}
	st->dev = dev;

	/* ISP INPUT: our sensor's native Bayer layout at the requested size --
	 * see ALIF_ISP_INPUT_FOURCC's comment. */
	struct video_format in_fmt = {
		.type        = VIDEO_BUF_TYPE_INPUT,
		.pixelformat = ALIF_ISP_INPUT_FOURCC,
		.width       = cfg->width,
		.height      = cfg->height,
	};
	int err = video_set_format(dev, &in_fmt);
	if (err != 0) {
		_free_state(st);
		return _errno_to_alp(err);
	}

	/* v4.4 video API: caps/format carry an `enum video_buf_type type` the
	 * caller sets.  The ISP-Pico output EP (the processed frame the app
	 * consumes) is VIDEO_BUF_TYPE_OUTPUT. */
	struct video_caps vcaps = { .type = VIDEO_BUF_TYPE_OUTPUT };
	err                     = video_get_caps(dev, &vcaps);
	if (err != 0 && err != -ENOSYS) {
		_free_state(st);
		return _errno_to_alp(err);
	}

	if (cfg->format == ALP_PIXFMT_RGB565) {
		st->convert_to_rgb565 = true;
		if (!_negotiate_output_fmt(dev,
		                           &vcaps,
		                           _rgb565_yuv_candidates,
		                           ARRAY_SIZE(_rgb565_yuv_candidates),
		                           cfg->width,
		                           cfg->height,
		                           &st->fmt)) {
			_free_state(st);
			return ALP_ERR_OUT_OF_RANGE;
		}
		st->isp_out_fourcc = st->fmt.pixelformat;
	} else {
		uint32_t want_fourcc = _to_video_fourcc(cfg->format);
		if (want_fourcc == 0u ||
		    !_negotiate_output_fmt(
		        dev, &vcaps, &want_fourcc, 1u, cfg->width, cfg->height, &st->fmt)) {
			_free_state(st);
			return ALP_ERR_OUT_OF_RANGE;
		}
	}

	/* AWB + AE: standard Zephyr video ctrls on the ISP device
	 * (isp_pico.c's isp_set_ctrl(), the same chain the sensor/csi/cam
	 * ctrls use). Both now equal the driver's own default (AWB=1,
	 * EXPOSURE_AUTO=AUTO -- matches the calibration's own OP_TYPE_AUTO
	 * 3A, hal_alif patch 0009), so video_set_ctrl() no-ops them; set
	 * explicitly anyway, before the first video_stream_start(), as
	 * documented intent and so a future driver default change can't
	 * silently leave this backend's 3A off. Best-effort, same as
	 * examples/aen/aen-isp-ov5647-capture: a driver build without the
	 * WB/AE library modules just leaves the ctrl unset. */
	struct video_control awb_ctrl = { .id = VIDEO_CID_AUTO_WHITE_BALANCE, .val = 1 };
	(void)video_set_ctrl(dev, &awb_ctrl);
	struct video_control ae_ctrl = { .id = VIDEO_CID_EXPOSURE_AUTO, .val = VIDEO_EXPOSURE_AUTO };
	(void)video_set_ctrl(dev, &ae_ctrl);

#if DT_NODE_EXISTS(DT_NODELABEL(ov5647))
	/* Request the caller's fps (issue #2276: this used to hard-code 10 fps
	 * here, silently ignoring cfg->fps).  cfg->fps == 0 means "backend
	 * default" (see <alp/camera.h>'s alp_camera_config_t::fps doc) -- NOT
	 * the same convention width/height use just above (those are a
	 * "you must choose" sentinel that alp_camera_open() rejects outright;
	 * an unset fps is not an error, it just falls back to this backend's
	 * own default of 10 fps, the OV5647's lowest supported rate
	 * (ov5647_framerates[] in ov5647.c) -- a *choice* that buys AE the
	 * most exposure headroom in a dim scene, not a hardware floor.
	 * ov5647_set_frmival() then picks the closest of {10, 15, 30, 45, 60,
	 * 90, 120} it can actually reach at this mode (VTS-clamped), so read
	 * the result back rather than assume the request landed exactly.
	 *
	 * isp_pico.c can't be asked generically here: it derives its own AE
	 * envelope from video_get_frmival(config->controller, ...) (isp ->
	 * cam -> csi -> sensor chain, sensor-agnostic -- see that file's AE
	 * comment), but `controller` is private to its driver config, not
	 * reachable from this backend, and isp_pico's video_driver_api
	 * doesn't implement .set_frmival/.get_frmival itself. So this stays
	 * scoped to the OV5647 shield's own DT node; a future sensor on this
	 * ISP path needs its own branch here.
	 *
	 * The sensor driver owns the exposure ceiling: ov5647_set_ctrl_exposure()
	 * (ov5647.c) clamps every VIDEO_CID_EXPOSURE write to the active mode's
	 * VTS - 4 lines. */
	const struct device *sensor_dev = DEVICE_DT_GET(DT_NODELABEL(ov5647));
	if (device_is_ready(sensor_dev)) {
		uint8_t              requested_fps = (cfg->fps != 0u) ? cfg->fps : 10u;
		struct video_frmival frmival       = { .numerator = 1, .denominator = requested_fps };
		int                  rc            = video_set_frmival(sensor_dev, &frmival);

		if (rc != 0) {
			LOG_WRN("camera%u: video_set_frmival(%u fps) failed: rc=%d",
			        cfg->camera_id,
			        requested_fps,
			        rc);
		}

		struct video_frmival actual = { 0 };
		if (video_get_frmival(sensor_dev, &actual) == 0 && actual.denominator > 0) {
			/* Print the settled interval as num/den rather than a
			 * truncating denominator/numerator division -- a
			 * non-integer or sub-1-fps settled rate would otherwise
			 * print a misleading rounded (or zero) fps. The request
			 * was always {.numerator = 1, .denominator =
			 * requested_fps}, so compare against that rather than
			 * requested_fps alone. */
			bool settled_as_requested =
			    (actual.numerator == 1u) && (actual.denominator == requested_fps);

			if (settled_as_requested) {
				LOG_DBG("camera%u: requested %u fps, sensor settled on %u/%u",
				        cfg->camera_id,
				        requested_fps,
				        actual.denominator,
				        actual.numerator);
			} else {
				LOG_INF("camera%u: requested %u fps, sensor settled on %u/%u",
				        cfg->camera_id,
				        requested_fps,
				        actual.denominator,
				        actual.numerator);
			}
		}
	} else {
		LOG_WRN("camera%u: OV5647 device not ready; fps request (%u) not applied",
		        cfg->camera_id,
		        cfg->fps);
	}
#endif

	uint8_t want = ARRAY_SIZE(st->vbufs);
	if (vcaps.min_vbuf_count > want) {
		_free_state(st);
		return ALP_ERR_OUT_OF_RANGE;
	}

	/* Per-buffer size: full frame size, not st->fmt.pitch * height -- see
	 * <zephyr/drivers/video/isp_frame_size.h> for why (pitch is the
	 * LUMA-only stride for planar/semi-planar YUV since isp_pico.c's
	 * isp_set_fmt() fix; the same helper sizes bytesused there). */
	uint32_t bytes_per_buf = alp_isp_frame_size(st->fmt.pixelformat, st->fmt.width, st->fmt.height);
	if (bytes_per_buf == 0u) {
		/* Real dimensions but a fourcc Zephyr's table can't size:
		 * refuse rather than under-allocate and let the ISP DMA past
		 * the end of the pool block. */
		_free_state(st);
		return ALP_ERR_NOSUPPORT;
	}

	/* Round the tail up to the pool's alignment too, so the last cache line
	 * of this buffer isn't shared with the next heap chunk. */
	bytes_per_buf = ROUND_UP(bytes_per_buf, CONFIG_VIDEO_BUFFER_POOL_ALIGN);

	for (uint8_t i = 0; i < want; ++i) {
		/* Pool-aligned, not video_buffer_alloc()'s sizeof(void *): see zephyr_video.c. */
		st->vbufs[i] =
		    video_buffer_aligned_alloc(bytes_per_buf, CONFIG_VIDEO_BUFFER_POOL_ALIGN, K_NO_WAIT);
		if (st->vbufs[i] == NULL) {
			/* Pool exhausted: give back vbufs[0..i-1] (already
			 * enqueued) before failing (#246). */
			_release_vbufs(st);
			_free_state(st);
			return ALP_ERR_NOMEM;
		}
		st->vbufs[i]->type = VIDEO_BUF_TYPE_OUTPUT;
		err                = video_enqueue(dev, st->vbufs[i]);
		if (err != 0) {
			/* Mid-loop enqueue failure: vbufs[0..i-1] sit in the
			 * driver's queue and vbufs[i] is loose -- release them
			 * all instead of leaking the pool (#246). */
			_release_vbufs(st);
			_free_state(st);
			return _errno_to_alp(err);
		}
	}
	st->vbuf_count = want;

	if (st->convert_to_rgb565) {
		uint32_t rgb565_bytes = ROUND_UP((uint32_t)cfg->width * cfg->height * sizeof(uint16_t),
		                                 CONFIG_VIDEO_BUFFER_POOL_ALIGN);
		st->rgb565_vbuf =
		    video_buffer_aligned_alloc(rgb565_bytes, CONFIG_VIDEO_BUFFER_POOL_ALIGN, K_NO_WAIT);
		if (st->rgb565_vbuf == NULL) {
			_release_vbufs(st);
			_free_state(st);
			return ALP_ERR_NOMEM;
		}
	}

	state->be_data = st;
	/* base_caps stays 0 so the surface ABI is reflected exactly; the
	 * ISP-present cap bit is advertised once cap_instance.h allocates it. */
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t isp_start(alp_camera_backend_state_t *state)
{
	alp_alif_isp_pico_state_t *st = (alp_alif_isp_pico_state_t *)state->be_data;
	if (st == NULL) {
		return ALP_ERR_NOT_READY;
	}
	if (st->streaming) {
		return ALP_OK;
	}
	/* Alif's own settle delay between the last buffer enqueue (isp_open())
	 * and the first video_stream_start() -- examples/aen/aen-isp-ov5647-
	 * capture mirrors this exactly; no smaller value is bench-proven. */
	k_msleep(1000);
	int err = video_stream_start(st->dev, VIDEO_BUF_TYPE_OUTPUT);
	if (err == 0) {
		st->streaming = true;
	}
	return _errno_to_alp(err);
}

static alp_status_t isp_stop(alp_camera_backend_state_t *state)
{
	alp_alif_isp_pico_state_t *st = (alp_alif_isp_pico_state_t *)state->be_data;
	if (st == NULL) {
		return ALP_ERR_NOT_READY;
	}
	if (!st->streaming) {
		return ALP_OK;
	}
	int err = video_stream_stop(st->dev, VIDEO_BUF_TYPE_OUTPUT);
	if (err == 0) {
		st->streaming = false;
	}
	return _errno_to_alp(err);
}

static alp_status_t
isp_capture(alp_camera_backend_state_t *state, alp_camera_frame_t *out, uint32_t timeout_ms)
{
	alp_alif_isp_pico_state_t *st = (alp_alif_isp_pico_state_t *)state->be_data;
	if (st == NULL) {
		return ALP_ERR_NOT_READY;
	}
	if (!st->streaming) {
		return ALP_ERR_NOT_READY;
	}

	/* isp_pico.c auto-stops once its incoming-buffer fifo empties (only
	 * CONFIG_ALP_SDK_CAMERA_ALIF_ISP_VBUF_COUNT buffers total, and one is
	 * checked out between this call and release()) and only restarts when
	 * this app calls video_stream_start() again (isp_pico.c's own model,
	 * no driver-driven restart of its own) -- re-arm before every dequeue,
	 * mirroring examples/aen/aen-isp-ov5647-capture's per-frame loop.
	 * -EBUSY just means the driver hadn't auto-stopped yet since the last
	 * call; either way there's nothing to do but keep going. */
	/* Hand every already-completed (stale) buffer straight back to the
	 * ISP before waiting: a viewfinder wants the NEXT frame, and a stale
	 * buffer parked in the driver's done-queue is one the MI can't fill --
	 * leave them there and the IN-FIFO starves, the driver auto-stops, and
	 * every restart costs time and loses frames -- exactly one frame per
	 * restart in the worst case (bench run 155). */
	struct video_buffer *stale = NULL;
	int                  err;

	/* A failed re-enqueue drops just that one buffer and keeps draining
	 * the rest -- returning here mid-loop would leave any buffer still
	 * behind `stale` parked in the driver's done-queue, the exact
	 * starve-and-auto-stop this loop exists to prevent (see above). In
	 * practice this never happens: isp_enqueue() (isp_pico.c) only ever
	 * fails on an 8-byte-misaligned buffer address, and `stale` just came
	 * back from this same driver's own done-queue -- it was already
	 * enqueued successfully once at this exact address, so it cannot
	 * newly become misaligned here. */
	while (video_dequeue(st->dev, &stale, K_NO_WAIT) == 0 && stale != NULL) {
		err = video_enqueue(st->dev, stale);
		ARG_UNUSED(err);
		stale = NULL;
	}

	err = video_stream_start(st->dev, VIDEO_BUF_TYPE_OUTPUT);
	if (err != 0 && err != -EBUSY) {
		return _errno_to_alp(err);
	}

	k_timeout_t          t  = (timeout_ms == UINT32_MAX) ? K_FOREVER : K_MSEC(timeout_ms);
	struct video_buffer *vb = NULL;
	err                     = video_dequeue(st->dev, &vb, t);
	if (err != 0) {
		return _errno_to_alp(err);
	}
	if (vb == NULL) {
		return ALP_ERR_IO;
	}

	if (st->convert_to_rgb565) {
		/* Table-driven fast path (yuv_to_rgb565.h) -- deliberately NOT
		 * alp_yuv_to_rgb565() called per pixel. With
		 * CONFIG_ALP_SDK_CAMERA_ALIF_ISP_VBUF_COUNT raw buffers still
		 * queued to the ISP MI (this one dequeued, held here through
		 * the conversion), the fifo can absorb one held buffer for
		 * roughly (VBUF_COUNT-1) frame periods before it starves and
		 * the driver auto-stops -- at this backend's 10 fps default
		 * (see the frmival request in open()) that's ~100 ms of
		 * headroom per spare buffer (less at a higher requested fps),
		 * nowhere near enough for
		 * the naive per-pixel path (~1 s/frame at -Os, see this file's
		 * header) but comfortably inside this fast path's budget. */
		if (st->isp_out_fourcc == VIDEO_PIX_FMT_YUYV) {
			alp_yuyv_frame_to_rgb565(
			    vb->buffer, st->fmt.width, st->fmt.height, (uint16_t *)st->rgb565_vbuf->buffer);
		} else {
			alp_yuv420_frame_to_rgb565(
			    vb->buffer, st->fmt.width, st->fmt.height, (uint16_t *)st->rgb565_vbuf->buffer);
		}
		/* Only NOW give the raw ISP buffer back -- the other
		 * VBUF_COUNT-1 raw buffers stayed queued the whole time and
		 * kept the MI fed during the conversion above (see isp_open()
		 * and CONFIG_ALP_SDK_CAMERA_ALIF_ISP_VBUF_COUNT). */
		err = video_enqueue(st->dev, vb);
		if (err != 0) {
			return _errno_to_alp(err);
		}
		out->data         = st->rgb565_vbuf->buffer;
		out->size         = (size_t)st->fmt.width * st->fmt.height * sizeof(uint16_t);
		out->timestamp_us = (uint64_t)vb->timestamp * 1000ull;
		return ALP_OK;
	}

	out->data         = vb->buffer;
	out->size         = vb->bytesused;
	out->timestamp_us = (uint64_t)vb->timestamp * 1000ull;
	return ALP_OK;
}

static alp_status_t isp_release(alp_camera_backend_state_t *state, alp_camera_frame_t *frame)
{
	alp_alif_isp_pico_state_t *st = (alp_alif_isp_pico_state_t *)state->be_data;
	if (st == NULL) {
		return ALP_ERR_NOT_READY;
	}
	if (frame == NULL || frame->data == NULL) {
		return ALP_ERR_INVAL;
	}
	if (st->convert_to_rgb565) {
		/* The raw ISP buffer was already re-enqueued in isp_capture();
		 * frame->data is this handle's own rgb565_vbuf scratch buffer,
		 * which isp_capture() overwrites next frame -- nothing to give
		 * back to the driver here. */
		return (frame->data == st->rgb565_vbuf->buffer) ? ALP_OK : ALP_ERR_INVAL;
	}
	for (uint8_t i = 0; i < st->vbuf_count; ++i) {
		if (st->vbufs[i] != NULL && st->vbufs[i]->buffer == frame->data) {
			int err = video_enqueue(st->dev, st->vbufs[i]);
			return _errno_to_alp(err);
		}
	}
	return ALP_ERR_INVAL;
}

/* ============================================================== */
/* ISP configure path -- latches the requested config into        */
/* backend state.  The libisp parameter upload lands when the     */
/* hal_alif wrapper is bumped (FLAGGED version mismatch).          */
/* ============================================================== */

static alp_status_t isp_configure_isp(alp_camera_backend_state_t    *state,
                                      const alp_camera_isp_config_t *isp)
{
	alp_alif_isp_pico_state_t *st = (alp_alif_isp_pico_state_t *)state->be_data;
	if (st == NULL) {
		return ALP_ERR_NOT_READY;
	}
	if (isp == NULL) {
		return ALP_ERR_INVAL;
	}

	/* Latch verbatim.  isp_open() already drives the AWB/AE ctrls this
	 * config's auto_white_balance/auto_exposure fields describe (see its
	 * comment); the remaining fields (brightness/contrast/saturation/
	 * lens_shading/dead_pixel_correction/noise_reduction/auto_focus) have
	 * no isp_vsi_set_param() call site in this backend yet -- keep the
	 * op ALP_OK so an app that configures the ISP eagerly during init
	 * doesn't fail on the fields already covered by the controls. */
	st->cfg            = *isp;
	st->isp_configured = true;
	return ALP_OK;
}

static void isp_close(alp_camera_backend_state_t *state)
{
	alp_alif_isp_pico_state_t *st = (alp_alif_isp_pico_state_t *)state->be_data;
	if (st == NULL) {
		return;
	}
	st->streaming = false;
	/* Stop + drain + release every buffer this handle allocated --
	 * _release_vbufs stops the stream itself (harmless when already
	 * stopped), so the pool is whole again for the next open (#246). */
	_release_vbufs(st);
	if (st->rgb565_vbuf != NULL) {
		(void)video_buffer_release(st->rgb565_vbuf);
		st->rgb565_vbuf = NULL;
	}
	_free_state(st);
	state->be_data = NULL;
}

static const alp_camera_ops_t _ops = {
	.open          = isp_open,
	.start         = isp_start,
	.stop          = isp_stop,
	.capture       = isp_capture,
	.release       = isp_release,
	.configure_isp = isp_configure_isp,
	.close         = isp_close,
};

ALP_BACKEND_REGISTER(camera,
                     alif_isp_pico,
                     {
                         .silicon_ref = "alif:ensemble:e8",
                         .vendor      = "alif",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
