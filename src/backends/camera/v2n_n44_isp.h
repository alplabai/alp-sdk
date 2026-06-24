/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal state layout for the V2N N44 ISP backend.  NOT a
 * public header -- shared between src/backends/camera/v2n_n44_isp.c
 * (the backend body) and src/backends/ext/renesas/camera.c (the
 * vendor-extension surface declared in <alp/ext/renesas/camera.h>)
 * so the vendor-ext can latch its finer-grained knobs (3A windows,
 * per-channel gain tables, LSC LUT) into the same per-handle state
 * the backend already owns.
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

/** Inclusive pixel-coordinate rectangle used by the 3A window
 *  setters.  All four fields are pixel offsets from the active
 *  frame's top-left corner. */
typedef struct {
	uint16_t x;
	uint16_t y;
	uint16_t w;
	uint16_t h;
} alp_v2n_n44_isp_rect_t;

/** Backend's per-handle state.  Held by the dispatcher via
 *  state.be_data; allocated from a fixed pool in the backend
 *  source file. */
typedef struct {
	const struct device *dev;
	struct video_format  fmt;
	struct video_buffer *vbufs[CONFIG_ALP_SDK_CAMERA_V2N_N44_ISP_VBUF_COUNT];
	uint8_t              vbuf_count;
	bool                 streaming;
	bool                 in_use;
	bool                 isp_configured;
	/** Last latched portable ISP config.  Vendor-ext bodies may
     *  re-read this when interpreting their finer-grained knobs
     *  (e.g. apply the LSC LUT only when lens_shading=true). */
	alp_camera_isp_config_t cfg;
	/** Vendor-ext latched state (filled by
     *  src/backends/ext/renesas/camera.c).  Reads back via the
     *  same TU until the V2N N44 ISP register surface lands. */
	alp_v2n_n44_isp_rect_t region_3a[ALP_V2N_N44_ISP_3A_REGION_COUNT];
	bool                   region_3a_set[ALP_V2N_N44_ISP_3A_REGION_COUNT];
	/** Gain-table latched length per channel; the table contents
     *  live in customer-owned memory (the loader does not copy --
     *  the pointer is stashed and the real MMIO writes happen
     *  when the V2N N44 ISP driver grows DMA support).
     *  TBD: per-channel gain LUTs are uploaded via a DMA-backed
     *  control descriptor on N44 silicon (datasheet §18.5 --
     *  register addresses pending).  Until then the SDK only
     *  caches the pointer + length for the vendor-ext getter. */
	const uint16_t *gain_table[ALP_V2N_N44_ISP_CHANNEL_COUNT];
	uint16_t        gain_table_len[ALP_V2N_N44_ISP_CHANNEL_COUNT];
	/** Lens-shading-correction LUT latch.  Owned by customer
     *  memory; the backend only caches the pointer + length.
     *  TBD: real load lands when the V2N N44 ISP driver grows
     *  the LSC SRAM upload path (datasheet §18.6). */
	const uint16_t *lsc_lut;
	uint16_t        lsc_lut_len;
} alp_v2n_n44_isp_state_t;

#endif /* ALP_BACKENDS_CAMERA_V2N_N44_ISP_H */
