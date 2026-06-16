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

#ifndef CONFIG_ALP_SDK_CAMERA_ALIF_ISP_VBUF_COUNT
#define CONFIG_ALP_SDK_CAMERA_ALIF_ISP_VBUF_COUNT 2
#endif

/** Backend's per-handle state.  Held by the dispatcher via state.be_data;
 *  allocated from a fixed pool in the backend source file. */
typedef struct {
	const struct device *dev;
	struct video_format  fmt;
	struct video_buffer *vbufs[CONFIG_ALP_SDK_CAMERA_ALIF_ISP_VBUF_COUNT];
	uint8_t              vbuf_count;
	bool                 streaming;
	bool                 in_use;
	bool                 isp_configured;
	/** Last latched portable ISP config.  The matching libisp parameter
	 *  upload (VIDEO_CID_ALIF_ISP_SET -> struct isp_params) lands when the
	 *  hal_alif libisp wrapper is bumped to the version isp_pico.c targets
	 *  (see the FLAGGED version mismatch in zephyr/Kconfig). */
	alp_camera_isp_config_t cfg;
} alp_alif_isp_pico_state_t;

#endif /* ALP_BACKENDS_CAMERA_ALIF_ISP_PICO_H */
