/**
 * @file ext/renesas/camera.h
 * @brief Renesas RZ/V2N N44 camera ISP vendor-specific surface.
 *
 * Non-portable.  Include only when you've committed to Renesas
 * V2N silicon for the gated feature.  Every function in this
 * header verifies the handle's backend is Renesas before
 * touching hardware; calls on a non-Renesas handle return
 * @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC.
 *
 * Covers finer-grained ISP knobs the portable
 * @ref alp_camera_isp_config_t struct can't carry without
 * leaking V2N-specific values into the public surface (3A
 * window rectangles, per-channel gain tables, lens-shading-
 * correction LUTs).  The coarse on/off toggles for AE / AWB /
 * AF / LSC stay in the portable struct -- this header layers
 * the per-block tuning that customers running V2N silicon for
 * machine-vision flows reach for.
 *
 * Earns its spot per the vendor-ext audit rule (spec §3a): the
 * portable @ref alp_camera_isp_config_t struct can't carry
 * pixel-coordinate rectangles or kibibyte-scale LUT pointers
 * without forcing every backend to allocate equivalent fields,
 * which would distort the portable struct shape for SoCs without
 * an ISP block.
 *
 * @par Supported silicon: renesas:rzv2n:n44
 *
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      First Renesas camera vendor-extension header.  Promotes to
 *      [ABI-STABLE] when three vendor families ship extensions.
 */

#ifndef ALP_EXT_RENESAS_CAMERA_H
#define ALP_EXT_RENESAS_CAMERA_H

#include <stdint.h>

#include <alp/camera.h>
#include <alp/peripheral.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Compile-time presence marker -- used by example code to gate vendor calls. */
#define ALP_EXT_RENESAS_CAMERA_AVAILABLE 1

/** Which 3A loop a window rectangle targets.  All three loops
 *  are addressable independently; the N44 ISP keeps one rectangle
 *  per loop so a fresh set call replaces (not unions with) the
 *  prior rectangle. */
typedef enum {
	ALP_RENESAS_CAMERA_3A_AE  = 0, /**< Auto-exposure metering region. */
	ALP_RENESAS_CAMERA_3A_AWB = 1, /**< Auto-white-balance statistics. */
	ALP_RENESAS_CAMERA_3A_AF  = 2, /**< Auto-focus contrast window. */
} alp_renesas_camera_3a_region_t;

/** Bayer / per-channel slot the gain-table loader addresses.  The
 *  N44 ISP applies one curve per channel before AWB integration;
 *  callers wanting a single curve across all channels load the
 *  same table four times. */
typedef enum {
	ALP_RENESAS_CAMERA_CHANNEL_R  = 0, /**< Red channel. */
	ALP_RENESAS_CAMERA_CHANNEL_GR = 1, /**< Green pixels on red-filtered rows. */
	ALP_RENESAS_CAMERA_CHANNEL_GB = 2, /**< Green pixels on blue-filtered rows. */
	ALP_RENESAS_CAMERA_CHANNEL_B  = 3, /**< Blue channel. */
} alp_renesas_camera_channel_t;

/** Pixel-coordinate rectangle.  All four fields measured from
 *  the active frame's top-left corner; (w, h) cap at the
 *  configured sensor resolution and a value of zero rejects
 *  with @ref ALP_ERR_INVAL. */
typedef struct {
	uint16_t x; /**< Left edge in pixels from the active frame's top-left. */
	uint16_t y; /**< Top edge in pixels from the active frame's top-left. */
	uint16_t w; /**< Width in pixels; 0 rejects with @ref ALP_ERR_INVAL. */
	uint16_t h; /**< Height in pixels; 0 rejects with @ref ALP_ERR_INVAL. */
} alp_renesas_camera_rect_t;

/**
 * @brief Place a 3A statistics window inside the active frame.
 *
 * @par Supported silicon: renesas:rzv2n:n44
 *
 * Lets callers move the AE / AWB / AF metering rectangle off
 * the default centre-weighted position for spot-meter style
 * flows.  Safe to call before or after @ref alp_camera_start;
 * the backend latches the rectangle and applies on the next
 * frame boundary.
 *
 * Bypasses @ref alp_camera_configure_isp on purpose -- the
 * portable struct carries no rectangle fields and growing it to
 * include them would distort SoCs without an ISP block.
 *
 * @param camera  Handle from @ref alp_camera_open opened against
 *                Renesas V2N silicon.  Non-NULL.
 * @param region  Which 3A loop the rectangle targets.
 * @param rect    Pixel-coordinate rectangle.  Non-NULL; rejects
 *                w == 0 or h == 0 with @ref ALP_ERR_INVAL.
 *
 * @return @ref ALP_OK on success;
 *         @ref ALP_ERR_INVAL on NULL handle / NULL rect /
 *           zero-sized rect / unknown region enum;
 *         @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC on non-Renesas backend;
 *         @ref ALP_ERR_NOT_READY if the camera isn't open.
 */
alp_status_t alp_renesas_camera_isp_3a_window_set(alp_camera_t                    *camera,
                                                  alp_renesas_camera_3a_region_t   region,
                                                  const alp_renesas_camera_rect_t *rect);

/**
 * @brief Load a per-channel gain LUT into the N44 ISP.
 *
 * @par Supported silicon: renesas:rzv2n:n44
 *
 * Each entry is a Q4.12 gain value (1.0 == 4096) applied to the
 * matching colour channel before AWB integration.  The table is
 * referenced -- not copied -- so the caller must keep the buffer
 * alive until either the camera handle closes or a subsequent
 * load supersedes it.  This avoids a hot-path copy for the
 * typical 256 / 512 / 1024-entry curves customers ship.
 *
 * @param camera   Handle from @ref alp_camera_open opened against
 *                 Renesas V2N silicon.  Non-NULL.
 * @param channel  Which colour channel the curve applies to.
 * @param table    Pointer to a contiguous uint16_t array.
 *                 Non-NULL.
 * @param len      Number of entries; 16..1024 inclusive.  The
 *                 N44 ISP rejects lengths outside this range
 *                 (datasheet r01uh1003ej §18.5).
 *
 * @return @ref ALP_OK on success;
 *         @ref ALP_ERR_INVAL on NULL / out-of-range arguments;
 *         @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC on non-Renesas backend;
 *         @ref ALP_ERR_NOT_READY if the camera isn't open.
 */
alp_status_t alp_renesas_camera_isp_gain_table_load(alp_camera_t                *camera,
                                                    alp_renesas_camera_channel_t channel,
                                                    const uint16_t              *table,
                                                    uint16_t                     len);

/**
 * @brief Load the lens-shading-correction LUT.
 *
 * @par Supported silicon: renesas:rzv2n:n44
 *
 * The N44 ISP applies a 2D vignetting-correction surface stored
 * as a flattened uint16_t grid; each entry is a Q4.12 gain
 * applied to the matching grid cell.  Grid dimensions are
 * implementation-defined -- callers pass the total cell count
 * via @p len.  As with the gain table the buffer is referenced
 * (not copied); keep it alive until close.
 *
 * @param camera  Handle from @ref alp_camera_open opened against
 *                Renesas V2N silicon.  Non-NULL.
 * @param lut     Pointer to the flattened LUT.  Non-NULL.
 * @param len     Total number of grid cells; 64..4096 inclusive.
 *
 * @return @ref ALP_OK / @ref ALP_ERR_INVAL /
 *         @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC /
 *         @ref ALP_ERR_NOT_READY.
 */
alp_status_t
alp_renesas_camera_isp_lsc_lut_load(alp_camera_t *camera, const uint16_t *lut, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* ALP_EXT_RENESAS_CAMERA_H */
