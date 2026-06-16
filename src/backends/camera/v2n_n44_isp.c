/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Renesas RZ/V2N N44 ISP-aware camera backend.
 *
 * Wraps the same Zephyr drivers/video/ sensor + buffer plumbing
 * the portable zephyr_video backend uses (DT alias alp-camera0..3
 * + video_get_caps / video_set_format / video_enqueue / video_dequeue),
 * but advertises the N44 on-die ISP block via the configure_isp op
 * so apps that opt into the higher-priority backend can drive AE /
 * AWB / AF + the picture-tuning offsets through the same vtable.
 *
 * Why a separate backend rather than a Kconfig switch on the
 * portable one:
 *   1. ISP register pokes are V2N-specific; the portable backend
 *      stays clean of vendor-only register addresses.
 *   2. priority=100 on silicon_ref="renesas:rzv2n:n44" overrides
 *      the zephyr_video registration (priority 50) on V2N builds
 *      while keeping the portable backend the natural default
 *      everywhere else.
 *
 * Stub vs real split (commit body documents the boundary):
 *   - The sensor pipeline (open / start / stop / capture / release /
 *     close) routes through Zephyr's video API verbatim -- those
 *     functions are NOT stubs.  When the V2N N44 SoC port wires
 *     its MIPI CSI-2 IP up to drivers/video/, these calls go to
 *     real silicon for free.
 *   - configure_isp() validates the input, latches the config into
 *     backend state, and returns ALP_OK -- the actual register
 *     poke (toggling the AE / AWB / AF enable bits in the N44 ISP
 *     control registers) is left as a TBD pending the Renesas RZ/V2N
 *     ISP register map (datasheet section 18 "Image Signal Processor"
 *     in the V2N Hardware User's Manual r01uh1003ej -- once
 *     available the latched config flows into the matching MMIO
 *     writes).
 *   - The Renesas vendor-ext surface (alp/ext/renesas/camera.h:
 *     3A window rectangles, per-channel gain tables, LSC LUT)
 *     routes through this backend's latched state today and grows
 *     real MMIO writes when the N44 port lands.
 *
 * @par Tracking: github.com/alplabai/alp-sdk/issues/20 (sensor path)
 * @par Tracking: github.com/alplabai/alp-sdk/issues/21 (ISP path)
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
#include "v2n_n44_isp.h"

#ifndef CONFIG_ALP_SDK_CAMERA_V2N_N44_ISP_VBUF_COUNT
#define CONFIG_ALP_SDK_CAMERA_V2N_N44_ISP_VBUF_COUNT 2
#endif

#ifndef CONFIG_ALP_SDK_MAX_CAMERA_HANDLES
#define CONFIG_ALP_SDK_MAX_CAMERA_HANDLES 2
#endif

#define ALP_V2N_CAM_DEV_OR_NULL(idx)                                                               \
	COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_camera, idx))),                                \
	            (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_camera, idx)))),                               \
	            (NULL))

static const struct device *const _devs[] = {
	ALP_V2N_CAM_DEV_OR_NULL(0),
	ALP_V2N_CAM_DEV_OR_NULL(1),
	ALP_V2N_CAM_DEV_OR_NULL(2),
	ALP_V2N_CAM_DEV_OR_NULL(3),
};

static alp_v2n_n44_isp_state_t _state_pool[CONFIG_ALP_SDK_MAX_CAMERA_HANDLES];

static alp_v2n_n44_isp_state_t *_alloc_state(void)
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

static void _free_state(alp_v2n_n44_isp_state_t *s)
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

/* ============================================================== */
/* Sensor / capture path -- delegates to Zephyr drivers/video/.    */
/* No V2N-specific MMIO yet; the N44 SoC port wires its MIPI CSI-2 */
/* IP up to drivers/video/, at which point these calls drive real */
/* silicon unchanged.                                              */
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

	alp_v2n_n44_isp_state_t *st = _alloc_state();
	if (st == NULL) return ALP_ERR_NOMEM;
	st->dev = dev;

	struct video_caps vcaps = { 0 };
	int               err   = video_get_caps(dev, VIDEO_EP_OUT, &vcaps);
	if (err != 0 && err != -ENOSYS) {
		_free_state(st);
		return _errno_to_alp(err);
	}

	uint32_t want_fourcc    = _to_video_fourcc(cfg->format);
	bool     fmt_negotiated = false;
	if (want_fourcc != 0u && vcaps.format_caps != NULL) {
		for (const struct video_format_cap *fc = vcaps.format_caps; fc->pixelformat != 0u; ++fc) {
			if (fc->pixelformat != want_fourcc) continue;
			if (cfg->width < fc->width_min || cfg->width > fc->width_max) continue;
			if (cfg->height < fc->height_min || cfg->height > fc->height_max) continue;
			st->fmt.pixelformat = want_fourcc;
			st->fmt.width       = cfg->width;
			st->fmt.height      = cfg->height;
			st->fmt.pitch       = 0u;
			err                 = video_set_format(dev, VIDEO_EP_OUT, &st->fmt);
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
		(void)video_get_format(dev, VIDEO_EP_OUT, &st->fmt);
	}

	uint8_t want = ARRAY_SIZE(st->vbufs);
	if (vcaps.min_vbuf_count > want) {
		_free_state(st);
		return ALP_ERR_OUT_OF_RANGE;
	}

	uint32_t bytes_per_buf = (st->fmt.pitch != 0u)
	                             ? (st->fmt.pitch * st->fmt.height)
	                             : ((uint32_t)st->fmt.width * st->fmt.height * 2u);
	if (bytes_per_buf == 0u) bytes_per_buf = 64u;

	for (uint8_t i = 0; i < want; ++i) {
		st->vbufs[i] = video_buffer_alloc(bytes_per_buf);
		if (st->vbufs[i] == NULL) {
			for (uint8_t j = 0; j < i; ++j)
				st->vbufs[j] = NULL;
			_free_state(st);
			return ALP_ERR_NOMEM;
		}
		err = video_enqueue(dev, VIDEO_EP_OUT, st->vbufs[i]);
		if (err != 0) {
			_free_state(st);
			return _errno_to_alp(err);
		}
	}
	st->vbuf_count = want;

	state->be_data = st;
	/* Advertise the ISP-present capability so callers querying
     * alp_camera_capabilities() see a backend-specific flag set;
     * the flag value lives in the cap_instance.h ISP-present bit
     * once that bit is allocated (TBD: cap_instance flag bit for
     * "on-die ISP available").  Today base_caps stays 0 so the
     * v0.5 snapshot reflects the surface ABI exactly. */
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t isp_start(alp_camera_backend_state_t *state)
{
	alp_v2n_n44_isp_state_t *st = (alp_v2n_n44_isp_state_t *)state->be_data;
	if (st == NULL) return ALP_ERR_NOT_READY;
	if (st->streaming) return ALP_OK;
	int err = video_stream_start(st->dev);
	if (err == 0) st->streaming = true;
	return _errno_to_alp(err);
}

static alp_status_t isp_stop(alp_camera_backend_state_t *state)
{
	alp_v2n_n44_isp_state_t *st = (alp_v2n_n44_isp_state_t *)state->be_data;
	if (st == NULL) return ALP_ERR_NOT_READY;
	if (!st->streaming) return ALP_OK;
	int err = video_stream_stop(st->dev);
	if (err == 0) st->streaming = false;
	return _errno_to_alp(err);
}

static alp_status_t
isp_capture(alp_camera_backend_state_t *state, alp_camera_frame_t *out, uint32_t timeout_ms)
{
	alp_v2n_n44_isp_state_t *st = (alp_v2n_n44_isp_state_t *)state->be_data;
	if (st == NULL) return ALP_ERR_NOT_READY;
	if (!st->streaming) return ALP_ERR_NOT_READY;

	k_timeout_t          t   = (timeout_ms == UINT32_MAX) ? K_FOREVER : K_MSEC(timeout_ms);
	struct video_buffer *vb  = NULL;
	int                  err = video_dequeue(st->dev, VIDEO_EP_OUT, &vb, t);
	if (err != 0) return _errno_to_alp(err);
	if (vb == NULL) return ALP_ERR_IO;

	out->data         = vb->buffer;
	out->size         = vb->bytesused;
	out->timestamp_us = (uint64_t)vb->timestamp * 1000ull;
	return ALP_OK;
}

static alp_status_t isp_release(alp_camera_backend_state_t *state, alp_camera_frame_t *frame)
{
	alp_v2n_n44_isp_state_t *st = (alp_v2n_n44_isp_state_t *)state->be_data;
	if (st == NULL) return ALP_ERR_NOT_READY;
	if (frame == NULL || frame->data == NULL) return ALP_ERR_INVAL;
	for (uint8_t i = 0; i < st->vbuf_count; ++i) {
		if (st->vbufs[i] != NULL && st->vbufs[i]->buffer == frame->data) {
			int err = video_enqueue(st->dev, VIDEO_EP_OUT, st->vbufs[i]);
			return _errno_to_alp(err);
		}
	}
	return ALP_ERR_INVAL;
}

/* ============================================================== */
/* ISP configure path -- latches the requested config into backend */
/* state.  Real MMIO writes deferred to when the V2N N44 Zephyr    */
/* SoC port grows the ISP control-register surface.                 */
/* ============================================================== */

static alp_status_t isp_configure_isp(alp_camera_backend_state_t    *state,
                                      const alp_camera_isp_config_t *isp)
{
	alp_v2n_n44_isp_state_t *st = (alp_v2n_n44_isp_state_t *)state->be_data;
	if (st == NULL) return ALP_ERR_NOT_READY;
	if (isp == NULL) return ALP_ERR_INVAL;

	/* Latch verbatim; once the N44 port lands an ISP driver, the
     * latched values get translated into the matching control
     * register writes (datasheet r01uh1003ej §18 "Image Signal
     * Processor" -- TBD register addresses) at this point in the
     * call.  The vendor-ext surface
     * (include/alp/ext/renesas/camera.h) reads the same latched
     * state for finer-grained knobs (3A windows / gain tables /
     * LSC LUT). */
	st->cfg            = *isp;
	st->isp_configured = true;
	/* TBD: poke the AE / AWB / AF enable bits into the ISP control
     * register block when the V2N N44 Zephyr SoC port grows the
     * matching driver.  Keep the call ALP_OK today so apps that
     * configure the ISP eagerly during init don't fail. */
	return ALP_OK;
}

static void isp_close(alp_camera_backend_state_t *state)
{
	alp_v2n_n44_isp_state_t *st = (alp_v2n_n44_isp_state_t *)state->be_data;
	if (st == NULL) return;
	if (st->streaming) {
		(void)video_stream_stop(st->dev);
		st->streaming = false;
	}
	for (uint8_t i = 0; i < st->vbuf_count; ++i) {
		st->vbufs[i] = NULL;
	}
	st->vbuf_count = 0;
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
                     v2n_n44_isp,
                     {
                         .silicon_ref = "renesas:rzv2n:n44",
                         .vendor      = "renesas",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
