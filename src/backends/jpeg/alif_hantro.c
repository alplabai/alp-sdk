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
 * now makes this explicit via req->format (@ref ALP_PIXFMT_NV12,
 * <alp/peripheral.h>): this backend only accepts that format and treats
 * req->y_plane as the base of the already-NV12-laid-out buffer;
 * req->u_plane/v_plane are NOT consulted (NV12's interleaved U/V plane
 * lives inside the same y_plane allocation -- see the struct doc on
 * alp_jpeg_encode_req_t in <alp/jpeg.h>).  Callers that need genuine
 * planar Y/U/V on this SoM fall back to sw_baseline (priority 50) via
 * an explicit backend pick, or lay their frame out as NV12 to reach the
 * hardware path.
 *
 * ADR 0017 Tier-2 (interim; see jpeg_hantro_vc9000e.c for the retirement
 * note).  vendor-ext.  BENCH-VERIFIED on real E1M-AEN801 (Ensemble E8)
 * silicon: JPEG_SWREG0 reads back JPEG_HW_ID (0x90001000, HW version
 * 0x00c0c200) and alp_jpeg_encode() returns rc=0 out_len=935 for a 64x64
 * NV12 frame -- the 935-byte JPEG (SOI FF D8) was pulled off-target over
 * SWD and round-tripped through libjpeg to a correct 64x64 image.  The
 * earlier MemManage fault in jpeg_header_generation() from the
 * output-buffer handoff was fixed via video_import_buffer() (see
 * hantro_encode()) and is confirmed resolved by this run.
 */

#if defined(CONFIG_ALP_SDK_JPEG_ALIF_HANTRO)

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/video.h>
#include <zephyr/drivers/video/video_alif.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>
#include <zephyr/video/video.h> /* video_import_buffer() -- see hantro_encode(). */

#include <alp/backend.h>
#include <alp/jpeg.h>
#include <alp/peripheral.h>

#include "jpeg_ops.h"

LOG_MODULE_REGISTER(alp_jpeg_alif_hantro, CONFIG_LOG_DEFAULT_LEVEL);

#ifndef CONFIG_ALP_SDK_MAX_JPEG_HANDLES
#define CONFIG_ALP_SDK_MAX_JPEG_HANDLES 1
#endif

/*
 * DMA-reachable-buffer contract (DFP-confirmed, see the @note on
 * alp_jpeg_encode() in <alp/jpeg.h>): both Alif's own Driver_JPEG.c and this
 * driver's Zephyr port (jpeg_hantro_vc9000e.c) write the RAW caller pointer
 * into the Hantro AXI base registers (JPEG_SWREG8/12/13) -- no bounce
 * buffer, no CPU-local-to-system-bus translation.  That makes the M55's
 * ITCM/DTCM windows silently wrong destinations: they hold real data from
 * the CPU's point of view, but the JPEG block's AXI master cannot reach
 * them at those addresses (a global alias exists -- see the board dts
 * `global_base` comment above `&itcm`/`&dtcm` -- but nothing here would
 * rewrite the pointer to use it).  Reject them here, before programming the
 * hardware, rather than let the encode hang or corrupt memory.
 *
 * Ranges come from the DT itcm/dtcm nodes' `reg` (ensemble_rtss_he.dtsi),
 * not a hardcoded literal, so a core with different TCM sizing still gets
 * the right guard.
 */
#define _ITCM_BASE DT_REG_ADDR(DT_NODELABEL(itcm))
#define _ITCM_SIZE DT_REG_SIZE(DT_NODELABEL(itcm))
#define _DTCM_BASE DT_REG_ADDR(DT_NODELABEL(dtcm))
#define _DTCM_SIZE DT_REG_SIZE(DT_NODELABEL(dtcm))

static bool _range_overlaps(uintptr_t addr, size_t len, uintptr_t region_base, size_t region_size)
{
	uintptr_t addr_end   = addr + len;
	uintptr_t region_end = region_base + region_size;

	return (addr < region_end) && (addr_end > region_base);
}

/**
 * @brief True if [p, p+len) does NOT overlap a core-local TCM window.
 *
 * Only rejects the ITCM/DTCM local windows -- everything else (SRAM0/1,
 * MRAM, external RAM) is a valid HW-DMA destination and must pass, e.g.
 * the silicon-proven SRAM0 addresses 0x02000000 (out_buf) / 0x02002000
 * (nv12_buf) used by the bench example.
 */
static bool _is_dma_reachable(const void *p, size_t len)
{
	uintptr_t addr = (uintptr_t)p;

	if (_range_overlaps(addr, len, _ITCM_BASE, _ITCM_SIZE)) {
		return false;
	}
	if (_range_overlaps(addr, len, _DTCM_BASE, _DTCM_SIZE)) {
		return false;
	}
	return true;
}

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
	case -ENOBUFS:
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
		/* Only layout this HW block understands -- see the file-header
		 * note; ALP_PIXFMT_YUV420_PLANAR (fully-planar) has no HW
		 * equivalent here. */
		.pixfmt_mask = (1u << ALP_PIXFMT_NV12),
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
	/*
	 * This block programs a fixed VIDEO_PIX_FMT_NV12 (4:2:0) encode, so both
	 * the buffer layout (format) AND the chroma sampling (subsample) must be
	 * the values it actually honours -- validating format alone would let
	 * NV12 + subsample=422/400 through and silently emit 4:2:0.
	 */
	if (req->format != ALP_PIXFMT_NV12 || req->subsample != ALP_JPEG_SUBSAMPLE_420 ||
	    req->y_plane == NULL) {
		return ALP_ERR_NOSUPPORT;
	}

	/* NV12: one Y plane immediately followed, at stride*height, by an
	 * interleaved half-height UV plane -- see the struct doc on
	 * alp_jpeg_encode_req_t. Total footprint = stride*height*3/2. */
	uint32_t in_stride = (req->y_stride != 0u) ? req->y_stride : req->width;
	size_t   in_len    = (size_t)in_stride * req->height * 3u / 2u;

	if (!_is_dma_reachable(req->y_plane, in_len)) {
		LOG_ERR("hantro jpeg: input buffer %p is in core-local TCM; core-local "
		        "TCM is not reachable by the JPEG DMA master; place the buffer "
		        "in global SRAM",
		        req->y_plane);
		return ALP_ERR_NOSUPPORT;
	}
	if (!_is_dma_reachable(out_buf, out_cap)) {
		LOG_ERR("hantro jpeg: output buffer %p is in core-local TCM; core-local "
		        "TCM is not reachable by the JPEG DMA master; place the buffer "
		        "in global SRAM",
		        out_buf);
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

	/*
	 * Output-buffer handoff (fixes the silicon MPU fault: MMFAR=0x0,
	 * PC=jpeg_header_generation).  This v4.4 video subsystem's
	 * video_enqueue() (drivers/video/video_common.c) does NOT forward a
	 * caller's `struct video_buffer` to the driver: it copies only
	 * .type into ITS OWN framework-owned pool slot video_buf[buf->index]
	 * and hands the driver *that* struct instead.  The previous code
	 * built a plain stack-local video_buffer with .buffer=out_buf and
	 * an unset (so 0-defaulted) .index -- nothing had ever populated
	 * pool slot 0's .buffer, so the driver's jpeg_start_encode() read
	 * buf->buffer == NULL and faulted inside jpeg_header_generation().
	 *
	 * video_import_buffer() (<zephyr/video/video.h>) is the framework's
	 * supported way to hand it a caller-owned pointer without a copy:
	 * it claims a free pool slot, marks it VIDEO_MEMORY_EXTERNAL (so
	 * video_buffer_release() below frees the SLOT, not our caller's
	 * out_buf memory), and points that slot's .buffer at out_buf. Only
	 * .index / .type then need to travel through the local vbuf --
	 * .buffer/.size are already live on the pool slot the driver reads.
	 */
	uint16_t out_idx;
	err = video_import_buffer((uint8_t *)out_buf, out_cap, &out_idx);
	if (err != 0) {
		return _errno_to_alp(err);
	}

	/* video_import_buffer() sets the pool slot's .size = out_cap and resets
	 * .bytesused to 0.  The driver programs the HW output-size-limit register
	 * (JPEG_SWREG9) from .size (fixed in jpeg_hantro_vc9000e.c), so the
	 * capacity reaches the HW correctly -- no bytesused priming needed here. */
	struct video_buffer vbuf = {
		.type  = VIDEO_BUF_TYPE_OUTPUT,
		.index = out_idx,
	};
	err = video_enqueue(st->dev, &vbuf);
	if (err != 0) {
		(void)video_buffer_release(&vbuf); /* never reached the driver -- reclaim the slot. */
		return _errno_to_alp(err);
	}

	if (!st->streaming) {
		err = video_stream_start(st->dev, VIDEO_BUF_TYPE_OUTPUT);
		if (err != 0) {
			/* The imported slot is already latched into the driver's
			 * data->current_buf; releasing it here would race the
			 * driver's own view of the same struct, so it is
			 * intentionally left held on this error path -- see the
			 * repeat-encode caveat below hantro_encode(). */
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
		/* Timeout/error: the driver may still reference data->current_buf
		 * (dequeue only clears it on a clean return), so the pool slot is
		 * left held here too -- same single-encode-validated caveat. */
		return _errno_to_alp(err);
	}
	if (done == NULL) {
		return ALP_ERR_NOMEM;
	}
	if (done->bytesused > out_cap) {
		*out_len = done->bytesused; /* required size, when known -- see jpeg.h. */
		(void)video_buffer_release(done);
		return ALP_ERR_NOMEM;
	}

	*out_len = done->bytesused;
	(void)video_buffer_release(
	    done); /* clean success: reclaim the pool slot for the next encode. */
	return ALP_OK;
}

/*
 * ponytail: repeat-encode caveat.  st->streaming is latched true on the
 * FIRST successful call and never reset, so a second alp_jpeg_encode()
 * on this handle skips video_stream_start() and goes straight to
 * video_enqueue() -- untested (this backend is single-encode-validated
 * on real silicon so far).  If the driver ever rejects a mid-stream
 * video_set_format() with -EBUSY (format change between two encodes on
 * an already-streaming device), that surfaces as ALP_ERR_BUSY from this
 * function rather than a silent misencode -- a caller doing repeat
 * encodes at a FIXED width/height/format is unaffected.  Upgrade path:
 * stop+restart streaming per encode (or per format change) if a real
 * multi-encode workload needs it; not done here to avoid guessing at
 * the driver's actual re-arm behaviour without a bench to check it against.
 */

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
