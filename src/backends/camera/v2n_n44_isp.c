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
 *     functions are NOT stubs.  They resolve a device ONLY through
 *     the alp-camera0..3 DT aliases below, so they reach silicon
 *     exactly when a V2N board or overlay points one of those
 *     aliases at a real drivers/video/ device -- and not one step
 *     sooner.  No V2N board or overlay in this repo defines one
 *     today, so isp_open() below fails its _devs[] NULL check and
 *     alp_camera_open() on V2N hands back NULL with last_error =
 *     ALP_ERR_NOT_READY.  An earlier revision of this comment said
 *     the calls would go to real silicon "for free" once the SoC
 *     port landed; that undersold the gap by five separate missing
 *     facts -- see the DATA-GATED block below.
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
 * DATA-GATED -- what an alp-cameraN alias on V2N Zephyr still needs, and
 * why none of it can be written from this tree.  Tracked by alp-sdk #1149;
 * every claim below was checked against the pinned Zephyr v4.4.1 and the
 * hal_renesas revision that pin imports, not recalled:
 *   1. A CSI-2 receiver DRIVER.  Zephyr v4.4.1's drivers/video/ ships no
 *      Renesas RZ/V receiver at all -- video_renesas_ra_ceu.c is the
 *      RA-family parallel CEU, and dts/bindings/video/ carries CSI-2
 *      receiver bindings only for NXP (nxp,mipi-csi2rx.yaml).  There is
 *      no upstream binding to point a node at, so ADR 0017's
 *      consume-upstream rung has nothing to consume yet.
 *   2. Its reg base.  dts/arm/renesas/rz/rzv/r9a09g056.dtsi declares no
 *      csi2 / cru / isp node; hal_renesas's rzv2n bsp_slave_address.h
 *      carries no CRU entry; and metadata/socs/renesas/rzv2n/n44.json's
 *      peripheral_instances block covers i2c / uart / gpt / gtm only, so
 *      the board generator has no base to emit either.  Source for the
 *      real value: the RZ/V2N Hardware User's Manual r01uh1003ej, CRU +
 *      MIPI CSI-2 register chapters (the Renesas BSP reference dts also
 *      carries it -- neither ships in this repo).
 *   3. Its CM33 interrupt, which is NOT a datasheet constant here.
 *      hal_renesas's rzv2n bsp_irq_id.h lists CRU0_CSI2_LINK_INT_IRQSELn
 *      = 494 and CRU1_CSI2_LINK_INT_IRQSELn = 500 in IRQSELn_Type -- the
 *      IRQSEL multiplexer's SELECTOR numbers.  IRQn_Type, the enum that
 *      actually feeds a DT `interrupts` cell, holds no CRU vector at all;
 *      it ends at SEL126_IRQn = 479.  So the cell is a free choice of one
 *      SELn vector PLUS an IRQSEL programming step that no code in this
 *      tree performs.  Contrast the mbox1 node in
 *      zephyr/boards/alp/e1m_v2n101_m33_sm/, whose `interrupts = <293 2>`
 *      came straight out of IRQn_Type as MHU_MSG5_NS_IRQn.
 *   4. The sensor part.  A CSI-2 sensor node needs a real compatible, CCI
 *      address, lane count and link frequency.  The E1M-X carrier exposes
 *      bare CAM0 / CAM1 connectors: metadata/boards/e1m-x-evk.yaml sets
 *      `ov5640: false` and names no sensor anywhere.  The part is a
 *      product decision, not a value to be looked up.
 *   5. The carrier routing.  Which of the E1M-X edge connector's four
 *      CSI instances (metadata/e1m/pinout-x-v1.json, CSI0..CSI3, ten pins
 *      each) lands on which of this SoC's two CRUs, and which of the
 *      GD32-owned CAM_EN_LDO0..3 rails
 *      (metadata/e1m_modules/v2n/gd32-io-mcu-map.csv: PC3 / PE8 / PE7 /
 *      PE10) powers which connector.  metadata/pinmux/v2n.yaml carries no
 *      CSI lane row at all -- these are carrier-schematic facts, not
 *      public metadata.
 *
 * Guessing any of 2-5 produces a devicetree that builds clean and binds to
 * nothing, which then reads as reviewed.  Leave it unwritten.
 *
 * The configure_isp register poke above is blocked on the same manual's
 * ISP chapter, and rides the same issue.
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
#include "v2n_n44_isp.h"
#include "alp_slot_claim.h"

#ifndef CONFIG_ALP_SDK_CAMERA_V2N_N44_ISP_VBUF_COUNT
#define CONFIG_ALP_SDK_CAMERA_V2N_N44_ISP_VBUF_COUNT 2
#endif

#ifndef CONFIG_ALP_SDK_MAX_CAMERA_HANDLES
#define CONFIG_ALP_SDK_MAX_CAMERA_HANDLES 2
#endif

#define ALP_V2N_CAM_DEV_OR_NULL(idx) \
	COND_CODE_1(DT_NODE_HAS_STATUS(DT_ALIAS(_CONCAT(alp_camera, idx)), okay), \
	            (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_camera, idx)))), \
	            (NULL))

static const struct device *const _devs[] = {
	ALP_V2N_CAM_DEV_OR_NULL(0),
	ALP_V2N_CAM_DEV_OR_NULL(1),
	ALP_V2N_CAM_DEV_OR_NULL(2),
	ALP_V2N_CAM_DEV_OR_NULL(3),
};

static alp_v2n_n44_isp_state_t _state_pool[CONFIG_ALP_SDK_MAX_CAMERA_HANDLES];

/* issue #1115 round-2 dev review: claim atomically (in_use is the LAST
 * member; memset only the bytes ahead of it -- see v2n_n44_isp.h)
 * instead of the previous plain check-then-set. */
static alp_v2n_n44_isp_state_t *_alloc_state(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(_state_pool); ++i) {
		if (alp_slot_try_claim(&_state_pool[i].in_use)) {
			memset(&_state_pool[i], 0, offsetof(alp_v2n_n44_isp_state_t, in_use));
			return &_state_pool[i];
		}
	}
	return NULL;
}

static void _free_state(alp_v2n_n44_isp_state_t *s)
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
static void _release_vbufs(alp_v2n_n44_isp_state_t *st)
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

	/* The v4.4 video API names the endpoint with an `enum video_buf_type`
	 * carried ON the caps / format / buffer structs rather than as a
	 * separate argument.  The N44 ISP's capture side -- the processed
	 * frames the app consumes -- is VIDEO_BUF_TYPE_OUTPUT. */
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
			if (fc->pixelformat != want_fourcc) continue;
			if (cfg->width < fc->width_min || cfg->width > fc->width_max) continue;
			if (cfg->height < fc->height_min || cfg->height > fc->height_max) continue;
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
		/* get_format reads the endpoint named by fmt.type -- set it first. */
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
	 * RGB24 = 24 bpp, XRGB32 = 32 bpp).  A flat 2 B/px guess here
	 * under-allocates the RGB888 (3 B/px) and ARGB8888 (4 B/px) frames
	 * _to_video_fourcc() can negotiate, while the ISP DMA writes the full
	 * frame regardless (#245). */
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
	int err = video_stream_start(st->dev, VIDEO_BUF_TYPE_OUTPUT);
	if (err == 0) st->streaming = true;
	return _errno_to_alp(err);
}

static alp_status_t isp_stop(alp_camera_backend_state_t *state)
{
	alp_v2n_n44_isp_state_t *st = (alp_v2n_n44_isp_state_t *)state->be_data;
	if (st == NULL) return ALP_ERR_NOT_READY;
	if (!st->streaming) return ALP_OK;
	int err = video_stream_stop(st->dev, VIDEO_BUF_TYPE_OUTPUT);
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
	int                  err = video_dequeue(st->dev, &vb, t);
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
			int err = video_enqueue(st->dev, st->vbufs[i]);
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
                     v2n_n44_isp,
                     {
                         .silicon_ref = "renesas:rzv2n:n44",
                         .vendor      = "renesas",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
