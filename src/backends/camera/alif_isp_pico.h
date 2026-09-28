/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Alp Lab AB
 *
 * Internal state layout for the Alif Ensemble ISP-Pico camera backend.  NOT a
 * public header -- private to src/backends/camera/alif_isp_pico.c.  Mirrors the
 * V2N N44 ISP backend's per-handle state (v2n_n44_isp.h): the sensor/capture
 * pipeline routes through the portable Zephyr v4.4 video API, and the
 * configure_isp op latches the requested config into this state.
 *
 * Layout may change between SDK versions; customer code never reaches this
 * struct.
 */

#ifndef ALP_BACKENDS_CAMERA_ALIF_ISP_PICO_H
#define ALP_BACKENDS_CAMERA_ALIF_ISP_PICO_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/video.h>

#include <alp/camera.h>

/* Normally set by the ALP_SDK_CAMERA_ALIF_ISP_VBUF_COUNT Kconfig (default
 * 3, range 2-8 -- see zephyr/kconfigs/power-camera-display.kconfig for the
 * full rationale, including when an app may safely drop to 2). This
 * fallback only fires for a non-Kconfig build of this file (e.g. a host
 * unit test) where CONFIG_* is never defined. */
#ifndef CONFIG_ALP_SDK_CAMERA_ALIF_ISP_VBUF_COUNT
#define CONFIG_ALP_SDK_CAMERA_ALIF_ISP_VBUF_COUNT 3
#endif

/** Backend's per-handle state.  Held by the dispatcher via state.be_data;
 *  allocated from a fixed pool in the backend source file. */
/* in_use is the LAST member (issue #1115 round-2 dev review, mirrors
 * dsp/sw_fallback.c's struct dsp_be): the atomic claimant in
 * alif_isp_pico.c's _alloc_state() memsets only the bytes ahead of it,
 * so the claim (in_use false -> true) is never transiently undone by
 * the reset. */
typedef struct {
	const struct device *dev;
	struct video_format  fmt;
	struct video_buffer *vbufs[CONFIG_ALP_SDK_CAMERA_ALIF_ISP_VBUF_COUNT];
	uint8_t              vbuf_count;
	bool                 streaming;
	bool                 isp_configured;
	/** Last latched portable ISP config.  The matching libisp parameter
	 *  upload (VIDEO_CID_ALIF_ISP_SET -> struct isp_params) lands when the
	 *  hal_alif libisp wrapper is bumped to the version isp_pico.c targets
	 *  (see the FLAGGED version mismatch in zephyr/Kconfig). */
	alp_camera_isp_config_t cfg;
	/** Set at open() when the caller requested ALP_PIXFMT_RGB565 -- the
	 *  ISP MI never produces RGB565 (see supported_output_fmts in
	 *  isp_pico.c), so this backend negotiates a YUV MI output instead
	 *  and converts to RGB565 on the CPU in isp_capture(). */
	bool convert_to_rgb565;
	/** Which YUV layout was actually negotiated for the OUTPUT type when
	 *  convert_to_rgb565 is set (VIDEO_PIX_FMT_YUYV or _YUV420) -- tells
	 *  isp_capture() which unpack loop to run. */
	uint32_t isp_out_fourcc;
	/** Per-handle RGB565 scratch buffer, sized width*height*2 at open();
	 *  isp_capture() overwrites it every frame and hands its ->buffer
	 *  back as the frame's data pointer.  Not enqueued to the ISP
	 *  device -- it never leaves this handle. */
	struct video_buffer *rgb565_vbuf;
	bool                 in_use;
} alp_alif_isp_pico_state_t;

#endif /* ALP_BACKENDS_CAMERA_ALIF_ISP_PICO_H */
