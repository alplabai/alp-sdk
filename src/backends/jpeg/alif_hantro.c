/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif Ensemble E8 Hantro VC9000E hardware JPEG-encoder backend for
 * <alp/jpeg.h>.  Drives the vendored + v4.4-ported Zephyr video driver
 * (zephyr/drivers/video/jpeg_hantro_vc9000e.c, compatible
 * "verisilicon,hantro-vc9000e-jpeg", ADR 0017 Tier-2) through the DT
 * node `jpeg0` on Ensemble E8 silicon.
 *
 * Registers silicon_ref="alif:ensemble:e8" at priority 100 -- outranks
 * both the NOT_IMPLEMENTED stub (priority 0, zephyr_stub.c) and the
 * portable TooJpeg software fallback (priority 50, sw_baseline.c) on
 * AEN801/E8 builds; every other SoM keeps encoding through the software
 * fallback.
 *
 * The driver is a single-ended m2m encoder: one raw YUV frame in, one
 * JPEG byte stream out, driven entirely through the standard v4.4
 * drivers/video/ API plus two controls:
 *   - VIDEO_CID_JPEG_COMPRESSION_QUALITY (upstream JPEG control class)
 *     reprograms the HW quantization tables immediately (no read-back
 *     needed -- see jpeg_hantro_vc9000e_set_ctrl()).
 *   - VIDEO_CID_JPEG_INPUT_BUFFER (private CID, <zephyr/drivers/video/
 *     video_alif.h>) smuggles the raw input-frame pointer through the
 *     control's int32 value slot -- sign-extended round-trip, sound on
 *     the M55's 32-bit address space.
 *
 * IMPORTANT input-layout constraint: the HW block only understands
 * semi-planar 4:2:0 (NV12/NV21 -- one Y plane immediately followed, at
 * `pitch * height`, by an interleaved UV plane); it latches a SINGLE
 * base pointer for the whole frame.  The portable <alp/jpeg.h> request
 * carries separate y_plane/u_plane/v_plane pointers (fully planar) with
 * no HW equivalent here, so this backend only accepts
 * ALP_JPEG_SUBSAMPLE_420 and treats req->y_plane as the base of an
 * already-NV12-laid-out buffer; req->u_plane/v_plane are NOT consulted.
 * Callers that need genuine planar Y/U/V on this SoM fall back to
 * sw_baseline (priority 50) via an explicit backend pick, or lay their
 * frame out as NV12 to reach the hardware path.
 *
 * ADR 0017 Tier-2 (interim; see jpeg_hantro_vc9000e.c for the retirement
 * note).  vendor-ext.  BENCH-PENDING: build-only proof this task, no
 * AEN801 silicon run yet.
 */

#if defined(CONFIG_ALP_SDK_JPEG_ALIF_HANTRO)

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/video.h>
#include <zephyr/drivers/video/video_alif.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/jpeg.h>
#include <alp/peripheral.h>

#include "jpeg_ops.h"

#ifndef CONFIG_ALP_SDK_MAX_JPEG_HANDLES
#define CONFIG_ALP_SDK_MAX_JPEG_HANDLES 1
#endif

/** Per-handle backend state, held via alp_jpeg_backend_state_t::be_data. */
typedef struct {
	const struct device *dev;
	bool                 streaming;
	bool                 in_use;
} alif_hantro_jpeg_state_t;

static alif_hantro_jpeg_state_t _state_pool[CONFIG_ALP_SDK_MAX_JPEG_HANDLES];

static alif_hantro_jpeg_state_t *_alloc_state(void)
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

static void _free_state(alif_hantro_jpeg_state_t *st)
{
	if (st != NULL) {
		st->in_use = false;
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
	case -ETIMEDOUT:
		return ALP_ERR_TIMEOUT;
	case -EIO:
		return ALP_ERR_IO;
	case -ENOSPC:
		return ALP_ERR_NOMEM;
	case -ENOTSUP:
	case -ENOSYS:
		return ALP_ERR_NOSUPPORT;
	default:
		return ALP_ERR_IO;
	}
}

static alp_status_t hantro_open(const alp_jpeg_config_t  *cfg,
                                alp_jpeg_backend_state_t *state,
                                alp_jpeg_caps_t          *caps_out)
{
	(void)cfg;

	const struct device *dev = DEVICE_DT_GET(DT_NODELABEL(jpeg0));
	if (!device_is_ready(dev)) {
		return ALP_ERR_NOT_READY;
	}

	alif_hantro_jpeg_state_t *st = _alloc_state();
	if (st == NULL) {
		return ALP_ERR_NOMEM;
	}
	st->dev = dev;

	/* Ask the driver for its real bounds (jpeg_hantro_vc9000e_get_caps())
	 * instead of hand-picking numbers -- NV12 and NV21 share one
	 * width/height range in jpeg_hantro_vc9000e_format_caps[], sourced
	 * from CONFIG_VIDEO_JPEG_HANTRO_VC9000E_MAX_WIDTH/MAX_HEIGHT
	 * (Kconfig default 16384x16384, not a smaller silicon limit). */
	struct video_caps vcaps = { .type = VIDEO_BUF_TYPE_INPUT };
	int               err   = video_get_caps(dev, &vcaps);

	uint16_t max_w = UINT16_MAX;
	uint16_t max_h = UINT16_MAX;
	if (err == 0 && vcaps.format_caps != NULL && vcaps.format_caps[0].pixelformat != 0u) {
		max_w = (uint16_t)MIN(vcaps.format_caps[0].width_max, UINT16_MAX);
		max_h = (uint16_t)MIN(vcaps.format_caps[0].height_max, UINT16_MAX);
	}

	*caps_out = (alp_jpeg_caps_t){
		.hw_accelerated  = true,
		.mjpeg_supported = true,
		.max_width       = max_w,
		.max_height      = max_h,
		/* Only 4:2:0 (semi-planar NV12/NV21) is real on this HW --
		 * see the file-header note on the y_plane-as-NV12-base
		 * constraint.  Do NOT also advertise _400/_422: the driver's
		 * format_caps table lists only NV12/NV21. */
		.subsample_mask = (1u << ALP_JPEG_SUBSAMPLE_420),
	};

	state->be_data = st;
	return ALP_OK;
}

static alp_status_t hantro_encode(alp_jpeg_backend_state_t    *state,
                                  const alp_jpeg_encode_req_t *req,
                                  void                        *out_buf,
                                  size_t                       out_cap,
                                  size_t                      *out_len)
{
	alif_hantro_jpeg_state_t *st = (alif_hantro_jpeg_state_t *)state->be_data;
	if (st == NULL) {
		return ALP_ERR_NOT_READY;
	}
	if (req->subsample != ALP_JPEG_SUBSAMPLE_420 || req->y_plane == NULL) {
		return ALP_ERR_NOSUPPORT;
	}

	struct video_format fmt = {
		.type        = VIDEO_BUF_TYPE_INPUT,
		.pixelformat = VIDEO_PIX_FMT_NV12,
		.width       = req->width,
		.height      = req->height,
		.pitch       = (req->y_stride != 0u) ? req->y_stride : req->width,
	};
	int err = video_set_format(st->dev, &fmt);
	if (err != 0) {
		return _errno_to_alp(err);
	}

	struct video_control qctrl = {
		.id  = VIDEO_CID_JPEG_COMPRESSION_QUALITY,
		.val = req->quality,
	};
	err = video_set_ctrl(st->dev, &qctrl);
	if (err != 0) {
		return _errno_to_alp(err);
	}

	/* Latch the raw-frame pointer -- see the file-header note on the
	 * int32 round-trip.  Must happen AFTER set_format (harmless order
	 * either way) and BEFORE enqueue (jpeg_hantro_vc9000e_enqueue()
	 * rejects a NULL input_buffer with -EINVAL). */
	struct video_control ibctrl = {
		.id  = VIDEO_CID_JPEG_INPUT_BUFFER,
		.val = (int32_t)(uintptr_t)req->y_plane,
	};
	err = video_set_ctrl(st->dev, &ibctrl);
	if (err != 0) {
		return _errno_to_alp(err);
	}

	/* bytesused primes JPEG_SWREG9 (the HW output-size-limit register,
	 * see jpeg_start_encode()) with the caller's buffer capacity; the
	 * driver overwrites it with the real encoded length before this
	 * same struct comes back out of video_dequeue(). */
	struct video_buffer vbuf = {
		.type      = VIDEO_BUF_TYPE_OUTPUT,
		.buffer    = (uint8_t *)out_buf,
		.size      = (uint32_t)out_cap,
		.bytesused = (uint32_t)out_cap,
	};
	err = video_enqueue(st->dev, &vbuf);
	if (err != 0) {
		return _errno_to_alp(err);
	}

	if (!st->streaming) {
		err = video_stream_start(st->dev, VIDEO_BUF_TYPE_OUTPUT);
		if (err != 0) {
			return _errno_to_alp(err);
		}
		st->streaming = true;
	}

	/* ponytail: timeout is a wide-margin heuristic scaled off frame area,
	 * not a datasheet number -- the driver's Kconfig help text quotes
	 * "up to 2MP @ 200 FPS" (~5 ms/2MP); this floors at 200 ms and adds
	 * ~0.5 ms per kilopixel on top.  Upgrade path: add a timeout_ms field
	 * to alp_jpeg_encode_req_t if a caller ever needs a tighter deadline. */
	uint32_t             timeout_ms = 200u + ((uint32_t)req->width * req->height) / 2000u;
	struct video_buffer *done       = NULL;
	err                             = video_dequeue(st->dev, &done, K_MSEC(timeout_ms));
	if (err != 0) {
		return _errno_to_alp(err);
	}
	if (done == NULL || done->bytesused > out_cap) {
		return ALP_ERR_NOMEM;
	}

	*out_len = done->bytesused;
	return ALP_OK;
}

static void hantro_close(alp_jpeg_backend_state_t *state)
{
	alif_hantro_jpeg_state_t *st = (alif_hantro_jpeg_state_t *)state->be_data;
	if (st == NULL) {
		return;
	}
	if (st->streaming) {
		(void)video_stream_stop(st->dev, VIDEO_BUF_TYPE_OUTPUT);
		st->streaming = false;
	}
	_free_state(st);
	state->be_data = NULL;
}

static const alp_jpeg_ops_t _ops = {
	.open   = hantro_open,
	.encode = hantro_encode,
	.close  = hantro_close,
};

ALP_BACKEND_REGISTER(jpeg,
                     alif_hantro,
                     {
                         .silicon_ref = "alif:ensemble:e8",
                         .vendor      = "alif",
                         .base_caps   = 0u,
                         .priority    = 100u,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

#endif /* CONFIG_ALP_SDK_JPEG_ALIF_HANTRO */
