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
 * stub-vs-real split, but talks the UPSTREAM v4.4 video API (the v2n backend
 * still uses the pre-v4.4 endpoint-id helpers; this one uses the ported API:
 * video_get_caps(dev, &caps) with caps.type, video_set_format(dev, &fmt),
 * video_buffer_alloc(size, K_NO_WAIT), video_stream_start(dev, type)).
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
 *     NOT stubs.  Pointed at the vsi,isp-pico device (alp-camera0), they drive
 *     the real ISP m2m path the moment the hal_alif libisp wrapper is bumped to
 *     the version isp_pico.c targets.
 *   - configure_isp() validates the input, latches the config into backend
 *     state, and returns ALP_OK -- the matching libisp parameter upload
 *     (isp_pico.c's VIDEO_CID_ALIF_ISP_SET -> struct isp_params via
 *     isp_vsi_set_param) is left as a TBD pending the hal_alif wrapper bump
 *     (the 2025 wrapper does not export isp_vsi_set_param -- see the FLAGGED
 *     version mismatch in zephyr/Kconfig under VIDEO_ISP_VSI).
 *
 * ADR 0017 Tier-2, OPT-IN (CONFIG_ALP_SDK_CAMERA_ALIF_ISP, default n).
 * vendor-ext, BENCH-UNVERIFIED.  Runtime capture HW-blocked on this batch
 * (no camera sensor wired).
 *
 * @par Tracking: github.com/alplabai/alp-sdk/issues/21
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/camera.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "camera_ops.h"
#include "alif_isp_pico.h"

#ifndef CONFIG_ALP_SDK_MAX_CAMERA_HANDLES
#define CONFIG_ALP_SDK_MAX_CAMERA_HANDLES 2
#endif

#define ALP_ALIF_ISP_DEV_OR_NULL(idx)                                                              \
	COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_camera, idx))),                                \
	            (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_camera, idx)))),                               \
	            (NULL))

static const struct device *const _devs[] = {
	ALP_ALIF_ISP_DEV_OR_NULL(0),
	ALP_ALIF_ISP_DEV_OR_NULL(1),
	ALP_ALIF_ISP_DEV_OR_NULL(2),
	ALP_ALIF_ISP_DEV_OR_NULL(3),
};

static alp_alif_isp_pico_state_t _state_pool[CONFIG_ALP_SDK_MAX_CAMERA_HANDLES];

static alp_alif_isp_pico_state_t *_alloc_state(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(_state_pool); ++i) {
		if (!_state_pool[i].in_use) {
			memset(&_state_pool[i], 0, sizeof(_state_pool[i]));
			_state_pool[i].in_use = true;
			return &_state_pool[i];
		}
	}
	return NULL;
}

static void _free_state(alp_alif_isp_pico_state_t *s)
{
	if (s != NULL) {
		s->in_use = false;
	}
}

static alp_status_t _errno_to_alp(int err)
{
	switch (err) {
	case 0:
		return ALP_OK;
	case -EINVAL:
		return ALP_ERR_INVAL;
	case -EBUSY:
		return ALP_ERR_BUSY;
	case -EAGAIN:
		return ALP_ERR_TIMEOUT;
	case -ETIMEDOUT:
		return ALP_ERR_TIMEOUT;
	case -EIO:
		return ALP_ERR_IO;
	case -ENOTSUP:
	case -ENOSYS:
		return ALP_ERR_NOSUPPORT;
	default:
		return ALP_ERR_IO;
	}
}

static uint32_t _to_video_fourcc(alp_pixfmt_t fmt)
{
	switch (fmt) {
	case ALP_PIXFMT_RGB565:
		return VIDEO_PIX_FMT_RGB565;
	case ALP_PIXFMT_RGB888:
		return VIDEO_PIX_FMT_RGB24;
	case ALP_PIXFMT_ARGB8888:
		return VIDEO_PIX_FMT_XRGB32;
	default:
		return 0u;
	}
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
	const struct device *dev = _devs[cfg->camera_id];
	if (dev == NULL || !device_is_ready(dev)) {
		return ALP_ERR_NOT_READY;
	}

	alp_alif_isp_pico_state_t *st = _alloc_state();
	if (st == NULL) {
		return ALP_ERR_NOMEM;
	}
	st->dev = dev;

	/* v4.4 video API: caps/format carry an `enum video_buf_type type` the
	 * caller sets.  The ISP-Pico output EP (the processed frame the app
	 * consumes) is VIDEO_BUF_TYPE_OUTPUT. */
	struct video_caps vcaps = { .type = VIDEO_BUF_TYPE_OUTPUT };
	int               err   = video_get_caps(dev, &vcaps);
	if (err != 0 && err != -ENOSYS) {
		_free_state(st);
		return _errno_to_alp(err);
	}

	uint32_t want_fourcc    = _to_video_fourcc(cfg->format);
	bool     fmt_negotiated = false;
	if (want_fourcc != 0u && vcaps.format_caps != NULL) {
		for (const struct video_format_cap *fc = vcaps.format_caps; fc->pixelformat != 0u; ++fc) {
			if (fc->pixelformat != want_fourcc) {
				continue;
			}
			if (cfg->width < fc->width_min || cfg->width > fc->width_max) {
				continue;
			}
			if (cfg->height < fc->height_min || cfg->height > fc->height_max) {
				continue;
			}
			st->fmt.type        = VIDEO_BUF_TYPE_OUTPUT;
			st->fmt.pixelformat = want_fourcc;
			st->fmt.width       = cfg->width;
			st->fmt.height      = cfg->height;
			st->fmt.pitch       = 0u;
			err                 = video_set_format(dev, &st->fmt);
			if (err != 0) {
				_free_state(st);
				return _errno_to_alp(err);
			}
			fmt_negotiated = true;
			break;
		}
		if (!fmt_negotiated) {
			_free_state(st);
			return ALP_ERR_OUT_OF_RANGE;
		}
	} else {
		st->fmt.type = VIDEO_BUF_TYPE_OUTPUT;
		(void)video_get_format(dev, &st->fmt);
	}

	uint8_t want = ARRAY_SIZE(st->vbufs);
	if (vcaps.min_vbuf_count > want) {
		_free_state(st);
		return ALP_ERR_OUT_OF_RANGE;
	}

	/* Per-buffer size: prefer the driver-negotiated pitch; when the driver
	 * reports none, derive bytes-per-pixel from the negotiated fourcc via
	 * Zephyr's own format table (video_bits_per_pixel: RGB565 = 16 bpp,
	 * RGB24 = 24 bpp, XRGB32 = 32 bpp).  The previous flat 2 B/px guess
	 * under-allocated RGB888 (3 B/px) and ARGB8888 (4 B/px) frames (#245). */
	uint32_t bytes_per_buf = (st->fmt.pitch != 0u) ? (st->fmt.pitch * st->fmt.height)
	                                               : (((uint32_t)st->fmt.width * st->fmt.height *
	                                                   video_bits_per_pixel(st->fmt.pixelformat)) /
	                                                  BITS_PER_BYTE);
	if (bytes_per_buf == 0u) {
		if (st->fmt.width != 0u && st->fmt.height != 0u) {
			/* Real dimensions but a fourcc Zephyr's table can't size:
			 * refuse rather than under-allocate and let the ISP DMA
			 * past the end of the pool block. */
			_free_state(st);
			return ALP_ERR_NOSUPPORT;
		}
		/* No format negotiated at all (driver without get_format):
		 * keep open() alive with a minimal dummy allocation. */
		bytes_per_buf = 64u;
	}

	for (uint8_t i = 0; i < want; ++i) {
		st->vbufs[i] = video_buffer_alloc(bytes_per_buf, K_NO_WAIT);
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

	k_timeout_t          t   = (timeout_ms == UINT32_MAX) ? K_FOREVER : K_MSEC(timeout_ms);
	struct video_buffer *vb  = NULL;
	int                  err = video_dequeue(st->dev, &vb, t);
	if (err != 0) {
		return _errno_to_alp(err);
	}
	if (vb == NULL) {
		return ALP_ERR_IO;
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

	/* Latch verbatim.  TBD: translate the latched config into the ISP-Pico's
	 * VIDEO_CID_ALIF_ISP_SET control (a struct isp_params upload via the
	 * hal_alif libisp isp_vsi_set_param) once the hal_alif wrapper is bumped
	 * to the version isp_pico.c targets -- the locally vendored 2025 wrapper
	 * does NOT export isp_vsi_set_param (see the FLAGGED version mismatch in
	 * zephyr/Kconfig under VIDEO_ISP_VSI).  Keep the call ALP_OK today so apps
	 * that configure the ISP eagerly during init don't fail. */
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
