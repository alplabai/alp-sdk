/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal state layout for the V2N N44 ISP backend.  NOT a
 * public header -- shared between src/backends/camera/v2n_n44_isp.c
 * (the backend body) and src/backends/ext/renesas/camera.c (the
 * vendor-extension surface declared in <alp/ext/renesas/camera.h>)
 * so the vendor-ext reaches the same per-handle state.
 *
 * Layout may change between SDK versions; customer code never
 * reaches this struct.
 */

#ifndef ALP_BACKENDS_CAMERA_V2N_N44_ISP_H
#define ALP_BACKENDS_CAMERA_V2N_N44_ISP_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/drivers/video.h>

#include <alp/camera.h>

#ifndef CONFIG_ALP_SDK_CAMERA_V2N_N44_ISP_VBUF_COUNT
#define CONFIG_ALP_SDK_CAMERA_V2N_N44_ISP_VBUF_COUNT 2
#endif

/** Bayer / per-channel slots the vendor-ext gain-table loader
 *  addresses.  Mirrors the four colour-filter-array sites the
 *  V2N N44 ISP exposes per the Renesas datasheet §18.3 (TBD --
 *  enum value space stays portable to the customer). */
typedef enum {
	ALP_V2N_N44_ISP_CHANNEL_R  = 0,
	ALP_V2N_N44_ISP_CHANNEL_GR = 1,
	ALP_V2N_N44_ISP_CHANNEL_GB = 2,
	ALP_V2N_N44_ISP_CHANNEL_B  = 3,
	ALP_V2N_N44_ISP_CHANNEL_COUNT
} alp_v2n_n44_isp_channel_t;

/** 3A statistics windows.  The N44 ISP keeps one rectangle per
 *  loop (AE / AWB / AF); the vendor-ext lets callers move it
 *  off-centre for spot-meter style flows. */
typedef enum {
	ALP_V2N_N44_ISP_3A_REGION_AE  = 0,
	ALP_V2N_N44_ISP_3A_REGION_AWB = 1,
	ALP_V2N_N44_ISP_3A_REGION_AF  = 2,
	ALP_V2N_N44_ISP_3A_REGION_COUNT
} alp_v2n_n44_isp_3a_region_t;

/** Backend's per-handle state.  Held by the dispatcher via
 *  state.be_data; allocated from a fixed pool in the backend
 *  source file. */
/* in_use moved to the LAST member (issue #1115 round-2 dev review,
 * mirrors dsp/sw_fallback.c's struct dsp_be): the atomic claimant in
 * v2n_n44_isp.c's _alloc_state() memsets only the bytes ahead of it,
 * so the claim (in_use false -> true) is never transiently undone by
 * the reset. */
typedef struct {
	const struct device *dev;
	struct video_format  fmt;
	struct video_buffer *vbufs[CONFIG_ALP_SDK_CAMERA_V2N_N44_ISP_VBUF_COUNT];
	uint8_t              vbuf_count;
	bool                 streaming;
	bool                 in_use;
} alp_v2n_n44_isp_state_t;

#endif /* ALP_BACKENDS_CAMERA_V2N_N44_ISP_H */
