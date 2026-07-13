/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr drivers/video/ camera backend.  Used on any SoC
 * unless a vendor-specific backend (e.g. v2n_n44_isp) registers a
 * more specific silicon_ref match at higher priority.
 *
 * The portable surface mirrored:
 *   alp_camera_open      -> video_get_caps + format negotiation +
 *                           video_set_format + video_buffer_alloc x N +
 *                           video_enqueue x N (warm the queue ahead
 *                           of stream_start, see Zephyr docs on
 *                           min_vbuf_count).
 *   alp_camera_start     -> video_stream_start
 *   alp_camera_stop      -> video_stream_stop
 *   alp_camera_capture   -> video_dequeue (blocking with timeout)
 *   alp_camera_release   -> video_enqueue (return the buffer to the
 *                           driver's incoming queue for reuse)
 *   alp_camera_close     -> stop if running + video_buffer free path
 *   configure_isp        -> NOSUPPORT (the portable video class has
 *                           no in-line ISP knobs; vendor backends
 *                           ride on top to add the configure_isp op).
 *
 * Registered as silicon_ref="*" at priority 50 -- always wins over
 * the zephyr_stub fallback (priority 0).  Vendor-specific real
 * backends (v2n_n44_isp, future alif_mali_c55_isp) register at
 * priority 100 so they win on their matching silicon.
 *
 * DT-alias table mirrors the SPI / I2C precedent: alp-camera0..3.
 * Apps select the camera via cfg->camera_id; absent aliases stay
 * NULL and the open returns ALP_ERR_NOT_READY.
 *
 * @par Tracking: github.com/alplabai/alp-sdk/issues/20
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

#ifndef CONFIG_ALP_SDK_CAMERA_ZEPHYR_VIDEO_VBUF_COUNT
#define CONFIG_ALP_SDK_CAMERA_ZEPHYR_VIDEO_VBUF_COUNT 2
#endif

#define ALP_CAM_DEV_OR_NULL(idx) \
	COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_camera, idx))), \
	            (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_camera, idx)))), \
	            (NULL))

static const struct device *const _devs[] = {
	ALP_CAM_DEV_OR_NULL(0),
	ALP_CAM_DEV_OR_NULL(1),
	ALP_CAM_DEV_OR_NULL(2),
	ALP_CAM_DEV_OR_NULL(3),
};

/** Per-handle backend state.  Held inside the dispatcher's
 *  alp_camera struct via the `state.be_data` void * slot, allocated
 *  from a fixed pool sized to match CONFIG_ALP_SDK_MAX_CAMERA_HANDLES.
 *  The pool entries are reused via the in_use flag. */
typedef struct {
	const struct device *dev;
	struct video_format  fmt;
	struct video_buffer *vbufs[CONFIG_ALP_SDK_CAMERA_ZEPHYR_VIDEO_VBUF_COUNT];
	uint8_t              vbuf_count;
	bool                 streaming;
	bool                 in_use;
} alp_z_video_state_t;

#ifndef CONFIG_ALP_SDK_MAX_CAMERA_HANDLES
#define CONFIG_ALP_SDK_MAX_CAMERA_HANDLES 2
#endif

static alp_z_video_state_t _state_pool[CONFIG_ALP_SDK_MAX_CAMERA_HANDLES];

static alp_z_video_state_t *_alloc_state(void)
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

static void _free_state(alp_z_video_state_t *s)
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

/** Map the portable alp_pixfmt_t enum to a Zephyr video FourCC.
 *  Returns 0 if the format isn't expressible in the portable enum
 *  yet -- callers fall back to whatever the sensor's default
 *  pixelformat is (no set_format call). */
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

static alp_status_t z_open(const alp_camera_config_t  *cfg,
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

	alp_z_video_state_t *st = _alloc_state();
	if (st == NULL) {
		return ALP_ERR_NOMEM;
	}
	st->dev = dev;

	/* Probe the sensor's caps so we know the buffer line-stride to
     * use for video_buffer_alloc.  Treat -ENOSYS as success-with-
     * minimal-info -- some bridges (e.g. CSI-2 SerDes pairs) leave
     * get_caps unimplemented and only honour set_format. */
	struct video_caps vcaps = { 0 };
	int               err   = video_get_caps(dev, VIDEO_EP_OUT, &vcaps);
	if (err != 0 && err != -ENOSYS) {
		_free_state(st);
		return _errno_to_alp(err);
	}

	/* Walk the format_caps list -- accept the first entry whose
     * (pixelformat, width, height) bracket the requested config.
     * If no portable FourCC is requested or the list is empty the
     * sensor's default format stays in place. */
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
			st->fmt.pitch       = 0u; /* driver fills in via set_format */
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
		/* Caller didn't supply a portable format -- read back the
         * sensor's default so our buffer allocations match. */
		(void)video_get_format(dev, VIDEO_EP_OUT, &st->fmt);
	}

	/* Decide buffer count: clamp the configured pool to the
     * driver's min_vbuf_count when reported. */
	uint8_t want = ARRAY_SIZE(st->vbufs);
	if (vcaps.min_vbuf_count > want) {
		_free_state(st);
		return ALP_ERR_OUT_OF_RANGE;
	}

	/* Compute buffer size: pitch * height when pitch is known,
     * else fall back to width * height * 2 (RGB565 worst-case for
     * the formats the portable enum carries -- BGGR8 sensors stay
     * within this bound).  ARGB8888 paths get caught by the
     * pitch-known branch since set_format fills pitch in. */
	uint32_t bytes_per_buf = (st->fmt.pitch != 0u)
	                             ? (st->fmt.pitch * st->fmt.height)
	                             : ((uint32_t)st->fmt.width * st->fmt.height * 2u);
	if (bytes_per_buf == 0u) {
		/* No format negotiated at all -- avoid passing 0 to alloc. */
		bytes_per_buf = 64u;
	}

	for (uint8_t i = 0; i < want; ++i) {
		st->vbufs[i] = video_buffer_alloc(bytes_per_buf);
		if (st->vbufs[i] == NULL) {
			/* Roll back partial allocation. */
			for (uint8_t j = 0; j < i; ++j) {
				/* video_buffer_alloc has no free in upstream Zephyr;
                 * a v0.5 follow-up will revisit this when the upstream
                 * lifecycle stabilises.  Today the slab is sized to
                 * the pool count so leakage on the rollback path stays
                 * bounded. */
				st->vbufs[j] = NULL;
			}
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
	/* No special caps from the portable Zephyr video class -- ISP
     * gates stay off, vendor backends layer them on. */
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t z_start(alp_camera_backend_state_t *state)
{
	alp_z_video_state_t *st = (alp_z_video_state_t *)state->be_data;
	if (st == NULL) return ALP_ERR_NOT_READY;
	if (st->streaming) return ALP_OK; /* idempotent */
	int err = video_stream_start(st->dev);
	if (err == 0) st->streaming = true;
	return _errno_to_alp(err);
}

static alp_status_t z_stop(alp_camera_backend_state_t *state)
{
	alp_z_video_state_t *st = (alp_z_video_state_t *)state->be_data;
	if (st == NULL) return ALP_ERR_NOT_READY;
	if (!st->streaming) return ALP_OK;
	int err = video_stream_stop(st->dev);
	if (err == 0) st->streaming = false;
	return _errno_to_alp(err);
}

static alp_status_t
z_capture(alp_camera_backend_state_t *state, alp_camera_frame_t *out, uint32_t timeout_ms)
{
	alp_z_video_state_t *st = (alp_z_video_state_t *)state->be_data;
	if (st == NULL) return ALP_ERR_NOT_READY;
	if (!st->streaming) return ALP_ERR_NOT_READY;

	k_timeout_t t = (timeout_ms == UINT32_MAX) ? K_FOREVER : K_MSEC(timeout_ms);

	struct video_buffer *vb  = NULL;
	int                  err = video_dequeue(st->dev, VIDEO_EP_OUT, &vb, t);
	if (err != 0) return _errno_to_alp(err);
	if (vb == NULL) return ALP_ERR_IO;

	out->data = vb->buffer;
	out->size = vb->bytesused;
	/* Zephyr's video_buffer timestamp is milliseconds; expose as
     * microseconds to the portable surface so callers don't need
     * to know the upstream unit. */
	out->timestamp_us = (uint64_t)vb->timestamp * 1000ull;
	return ALP_OK;
}

static alp_status_t z_release(alp_camera_backend_state_t *state, alp_camera_frame_t *frame)
{
	alp_z_video_state_t *st = (alp_z_video_state_t *)state->be_data;
	if (st == NULL) return ALP_ERR_NOT_READY;
	if (frame == NULL || frame->data == NULL) return ALP_ERR_INVAL;

	/* Find the vbuf whose buffer pointer matches and re-enqueue it. */
	for (uint8_t i = 0; i < st->vbuf_count; ++i) {
		if (st->vbufs[i] != NULL && st->vbufs[i]->buffer == frame->data) {
			int err = video_enqueue(st->dev, VIDEO_EP_OUT, st->vbufs[i]);
			return _errno_to_alp(err);
		}
	}
	return ALP_ERR_INVAL;
}

static alp_status_t z_configure_isp(alp_camera_backend_state_t    *state,
                                    const alp_camera_isp_config_t *isp)
{
	(void)state;
	(void)isp;
	/* The portable Zephyr drivers/video/ class has no in-line ISP
     * surface.  Vendor backends (v2n_n44_isp, alif_mali_c55_isp)
     * register at higher priority on their matching silicon and
     * provide a real configure_isp body. */
	return ALP_ERR_NOSUPPORT;
}

static void z_close(alp_camera_backend_state_t *state)
{
	alp_z_video_state_t *st = (alp_z_video_state_t *)state->be_data;
	if (st == NULL) return;
	if (st->streaming) {
		(void)video_stream_stop(st->dev);
		st->streaming = false;
	}
	/* video_buffer_alloc has no upstream free counterpart yet --
     * we drop our references and trust the slab to recycle once
     * the handle slot is reused.  Pool is sized to the max handle
     * count so the worst-case footprint is bounded. */
	for (uint8_t i = 0; i < st->vbuf_count; ++i) {
		st->vbufs[i] = NULL;
	}
	st->vbuf_count = 0;
	_free_state(st);
	state->be_data = NULL;
}

static const alp_camera_ops_t _ops = {
	.open          = z_open,
	.start         = z_start,
	.stop          = z_stop,
	.capture       = z_capture,
	.release       = z_release,
	.configure_isp = z_configure_isp,
	.close         = z_close,
};

ALP_BACKEND_REGISTER(camera,
                     zephyr_video,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr",
                         .base_caps   = 0u,
                         .priority    = 50,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
