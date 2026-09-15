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
 *   alp_camera_close     -> video_stream_stop + drain-dequeue +
 *                           video_buffer_release x N (the pool is
 *                           whole again for the next open)
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
#include <stddef.h>
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

#include "alp_errno.h"
#include "camera_ops.h"
#include "alp_slot_claim.h"

#ifndef CONFIG_ALP_SDK_CAMERA_ZEPHYR_VIDEO_VBUF_COUNT
#define CONFIG_ALP_SDK_CAMERA_ZEPHYR_VIDEO_VBUF_COUNT 2
#endif

#define ALP_CAM_DEV_OR_NULL(idx) \
	COND_CODE_1(DT_NODE_HAS_STATUS(DT_ALIAS(_CONCAT(alp_camera, idx)), okay), \
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

/* issue #1115 round-2 dev review: claim atomically (in_use is the LAST
 * member; memset only the bytes ahead of it) instead of the previous
 * plain check-then-set scan. */
static alp_z_video_state_t *_alloc_state(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(_state_pool); ++i) {
		if (alp_slot_try_claim(&_state_pool[i].in_use)) {
			memset(&_state_pool[i], 0, offsetof(alp_z_video_state_t, in_use));
			return &_state_pool[i];
		}
	}
	return NULL;
}

static void _free_state(alp_z_video_state_t *s)
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

/* Release every video_buffer this handle acquired, getting the driver's
 * queue out of the way first.  video_stream_stop() implies a CANCEL flush
 * (video.h: `video_flush(dev, true)` moves everything the driver holds
 * from its incoming queue to the outgoing one as VIDEO_BUF_ABORTED), so a
 * stop + drain-dequeue detaches the buffers from the device before
 * video_buffer_release() returns them to the shared pool.  Releasing a
 * buffer the driver still queues would recycle a pool slot the device can
 * later hand back -- a stale pointer on the next open (#246).
 *
 * @param stop_stream  Call video_stream_stop() first.  The open()
 *                     rollback paths pass false: the stream was never
 *                     started there, and a driver that treats a
 *                     stop-while-idle as an error (or powers the sensor
 *                     down on it) should not see one.  z_close() passes
 *                     true unconditionally -- it needs the real stop. */
static void _release_vbufs(alp_z_video_state_t *st, bool stop_stream)
{
	struct video_buffer *vb = NULL;

	if (stop_stream) {
		(void)video_stream_stop(st->dev, VIDEO_BUF_TYPE_OUTPUT);
	}
	/* Bounded by ARRAY_SIZE(st->vbufs): that is the most buffers this
	 * handle can ever have queued, so a driver whose dequeue never
	 * returns non-zero (re-queues, or always "succeeds") cannot hang
	 * this close() path forever. */
	for (size_t i = 0; i < ARRAY_SIZE(st->vbufs); ++i) {
		if (video_dequeue(st->dev, &vb, K_NO_WAIT) != 0 || vb == NULL) {
			break;
		}
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
	 * get_caps unimplemented and only honour set_format.
	 *
	 * The v4.4 video API names the endpoint with an `enum video_buf_type`
	 * carried ON the caps / format / buffer structs rather than as a
	 * separate argument.  A camera's capture side -- the frames the app
	 * consumes -- is VIDEO_BUF_TYPE_OUTPUT. */
	struct video_caps vcaps = { .type = VIDEO_BUF_TYPE_OUTPUT };
	int               err   = video_get_caps(dev, &vcaps);
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
			st->fmt.type        = VIDEO_BUF_TYPE_OUTPUT;
			st->fmt.pixelformat = want_fourcc;
			st->fmt.width       = cfg->width;
			st->fmt.height      = cfg->height;
			st->fmt.pitch       = 0u; /* driver fills in via set_format */
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
		/* Caller didn't supply a portable format -- read back the
		 * sensor's default so our buffer allocations match.  get_format
		 * reads the endpoint named by fmt.type, so set it first. */
		st->fmt.type = VIDEO_BUF_TYPE_OUTPUT;
		(void)video_get_format(dev, &st->fmt);
	}

	/* Decide buffer count: clamp the configured pool to the
     * driver's min_vbuf_count when reported. */
	uint8_t want = ARRAY_SIZE(st->vbufs);
	if (vcaps.min_vbuf_count > want) {
		_free_state(st);
		return ALP_ERR_OUT_OF_RANGE;
	}

	/* Per-buffer size: prefer the driver-negotiated pitch; when the driver
	 * reports none, derive bytes-per-pixel from the negotiated fourcc via
	 * Zephyr's own format table (video_bits_per_pixel: RGB565 = 16 bpp,
	 * RGB24 = 24 bpp, XRGB32 = 32 bpp), rounded up so a table entry that
	 * isn't byte-aligned (e.g. the 10/12-bit packed Bayer formats) can't
	 * under-allocate by truncating.  The previous flat 2 B/px guess
	 * under-allocated RGB888 (3 B/px) and ARGB8888 (4 B/px) frames (#245,
	 * propagated to this backend in #1628). */
	uint32_t bytes_per_buf = (st->fmt.pitch != 0u)
	                             ? (st->fmt.pitch * st->fmt.height)
	                             : DIV_ROUND_UP((uint32_t)st->fmt.width * st->fmt.height *
	                                                video_bits_per_pixel(st->fmt.pixelformat),
	                                            BITS_PER_BYTE);
	if (bytes_per_buf == 0u) {
		if (st->fmt.pixelformat == VIDEO_PIX_FMT_JPEG && st->fmt.width != 0u &&
		    st->fmt.height != 0u) {
			/* Compressed formats have no fixed bits-per-pixel --
			 * video_bits_per_pixel() has no JPEG entry and returns 0.
			 * Fall back to the pre-#1628 flat w*h*2 heuristic: not a
			 * guaranteed compressed-frame ceiling, just the best a
			 * portable backend can do without a codec-specific max. */
			bytes_per_buf = (uint32_t)st->fmt.width * st->fmt.height * 2u;
		} else if (st->fmt.width != 0u && st->fmt.height != 0u) {
			/* Real dimensions but a fourcc Zephyr's table can't size:
			 * refuse rather than under-allocate and let the capture
			 * engine DMA past the end of the pool block. */
			_free_state(st);
			return ALP_ERR_NOSUPPORT;
		} else {
			/* No format negotiated at all (driver without
			 * get_format): fail loudly rather than open a handle
			 * whose first capture overruns a 64 B dummy buffer. */
			_free_state(st);
			return ALP_ERR_NOT_READY;
		}
	}

	for (uint8_t i = 0; i < want; ++i) {
		st->vbufs[i] = video_buffer_alloc(bytes_per_buf, K_NO_WAIT);
		if (st->vbufs[i] == NULL) {
			/* Pool exhausted: give back vbufs[0..i-1] (already
			 * enqueued) before failing (#246).  Stream was never
			 * started here, so don't stop it. */
			_release_vbufs(st, false);
			_free_state(st);
			return ALP_ERR_NOMEM;
		}
		st->vbufs[i]->type = VIDEO_BUF_TYPE_OUTPUT;
		err                = video_enqueue(dev, st->vbufs[i]);
		if (err != 0) {
			/* Mid-loop enqueue failure: vbufs[0..i-1] sit in the
			 * driver's queue and vbufs[i] is loose -- release them
			 * all instead of leaking the pool (#246).  Stream was
			 * never started here, so don't stop it. */
			_release_vbufs(st, false);
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
	int err = video_stream_start(st->dev, VIDEO_BUF_TYPE_OUTPUT);
	if (err == 0) st->streaming = true;
	return _errno_to_alp(err);
}

static alp_status_t z_stop(alp_camera_backend_state_t *state)
{
	alp_z_video_state_t *st = (alp_z_video_state_t *)state->be_data;
	if (st == NULL) return ALP_ERR_NOT_READY;
	if (!st->streaming) return ALP_OK;
	int err = video_stream_stop(st->dev, VIDEO_BUF_TYPE_OUTPUT);
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
	int                  err = video_dequeue(st->dev, &vb, t);
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
			int err = video_enqueue(st->dev, st->vbufs[i]);
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
	st->streaming = false;
	/* Stop + drain + release every buffer this handle allocated --
	 * _release_vbufs(st, true) stops the stream itself (harmless when
	 * already stopped), so the pool is whole again for the next open
	 * (#246). */
	_release_vbufs(st, true);
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
