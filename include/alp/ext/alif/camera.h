/**
 * @file ext/alif/camera.h
 * @brief Alif Ensemble Mali-C55 ISP vendor-specific surface.
 *
 * Non-portable.  Include only when you've committed to Alif
 * Ensemble silicon (E4 / E6 / E8 -- the variants that ship the
 * hardened Mali-C55 ISP fabric per the v0.5 AEN feature audit,
 * see docs/aen-feature-audit-2026-05.md §4.3).  Every function
 * in this header verifies the handle's backend is Alif before
 * touching hardware; calls on a non-Alif handle return
 * @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC.
 *
 * Covers the finer-grained ISP knobs the Mali-C55 exposes
 * beyond the portable @ref alp_camera_isp_config_t struct:
 *
 *   - 3A statistics windows (Mali-C55 carries 1024 weighted
 *     metering cells per loop; the API surface picks a coarser
 *     rectangle to match the Renesas V2N N44 ISP's surface).
 *   - per-channel gain LUTs.
 *   - lens-shading-correction LUT (the Mali-C55's "MESH" mode).
 *
 * Earns its spot per the vendor-ext audit rule (spec §3a): the
 * portable @ref alp_camera_isp_config_t struct can't carry
 * pixel-coordinate rectangles or kibibyte-scale LUT pointers
 * without forcing every backend (including AEN-family E3 / E5 /
 * E7 silicon without the Mali-C55 fabric) to allocate equivalent
 * fields.  Same shape as <alp/ext/renesas/camera.h> -- callers
 * targeting both V2N and AEN silicon get a near-identical API
 * surface; the vendor-handle gate keeps each call routed to the
 * matching hardware.
 *
 * @par Supported silicon: alif:ensemble:e4, alif:ensemble:e6, alif:ensemble:e8
 *      (E3 / E5 / E7 do not expose the Mali-C55 fabric; vendor
 *      packs may extend this list in a follow-up release.)
 *
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      Header lands ahead of the vendor pack body; every function
 *      returns @ref ALP_ERR_NOSUPPORT until the Alif HAL Mali-C55
 *      integration lands (same pattern as the OSPI SecAES and
 *      FlexSPI OTFAD precedents from Slice 6).  Promotes to
 *      [ABI-STABLE] when three vendor families ship extensions.
 */

#ifndef ALP_EXT_ALIF_CAMERA_H
#define ALP_EXT_ALIF_CAMERA_H

#include <stdint.h>

#include <alp/camera.h>
#include <alp/peripheral.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Compile-time presence marker -- used by example code to gate vendor calls. */
#define ALP_EXT_ALIF_CAMERA_AVAILABLE 1

/** Which 3A loop a window rectangle targets.  The Mali-C55
 *  honours one rectangle per loop; a fresh call replaces the
 *  prior rectangle. */
typedef enum {
	ALP_ALIF_CAMERA_3A_AE  = 0, /**< Auto-exposure metering region. */
	ALP_ALIF_CAMERA_3A_AWB = 1, /**< Auto-white-balance statistics. */
	ALP_ALIF_CAMERA_3A_AF  = 2, /**< Auto-focus contrast window. */
} alp_alif_camera_3a_region_t;

/** Bayer / per-channel slot the gain-table loader addresses. */
typedef enum {
	ALP_ALIF_CAMERA_CHANNEL_R  = 0,
	ALP_ALIF_CAMERA_CHANNEL_GR = 1,
	ALP_ALIF_CAMERA_CHANNEL_GB = 2,
	ALP_ALIF_CAMERA_CHANNEL_B  = 3,
} alp_alif_camera_channel_t;

/** Pixel-coordinate rectangle.  Layout intentionally mirrors
 *  @ref alp_renesas_camera_rect_t so customers spanning V2N and
 *  AEN silicon can share rectangle-computation code modulo the
 *  enum types. */
typedef struct {
	uint16_t x;
	uint16_t y;
	uint16_t w;
	uint16_t h;
} alp_alif_camera_rect_t;

/**
 * @brief Place a 3A statistics window inside the active frame.
 *
 * @par Supported silicon: alif:ensemble:e4, alif:ensemble:e6, alif:ensemble:e8
 *
 * Mirrors the Renesas knob's contract.  Mali-C55 weights the
 * matching 1024-cell statistics grid by the inclusive rectangle.
 *
 * @param camera  Handle from @ref alp_camera_open opened against
 *                an Alif E4 / E6 / E8 SoC.
 * @param region  Which 3A loop the rectangle targets.
 * @param rect    Pixel-coordinate rectangle.  Non-NULL; rejects
 *                w == 0 or h == 0 with @ref ALP_ERR_INVAL.
 *
 * @return @ref ALP_OK on success;
 *         @ref ALP_ERR_INVAL on NULL handle / NULL rect /
 *           zero-sized rect;
 *         @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC on non-Alif backend;
 *         @ref ALP_ERR_NOSUPPORT until the Alif HAL Mali-C55
 *           integration lands.
 */
alp_status_t alp_alif_camera_isp_3a_window_set(alp_camera_t                 *camera,
                                               alp_alif_camera_3a_region_t   region,
                                               const alp_alif_camera_rect_t *rect);

/**
 * @brief Load a per-channel gain LUT into the Mali-C55.
 *
 * @par Supported silicon: alif:ensemble:e4, alif:ensemble:e6, alif:ensemble:e8
 *
 * Each entry is a Q4.12 gain value applied to the matching
 * colour channel before AWB integration.  Reference-not-copy
 * semantics -- the caller must keep the buffer alive until close
 * or a subsequent load.
 *
 * @param camera   Handle from @ref alp_camera_open.
 * @param channel  Colour-channel slot the LUT applies to.
 * @param table    Pointer to a contiguous uint16_t array.
 * @param len      Number of entries; 16..1024 inclusive.
 *
 * @return @ref ALP_OK / @ref ALP_ERR_INVAL /
 *         @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC /
 *         @ref ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_alif_camera_isp_gain_table_load(alp_camera_t             *camera,
                                                 alp_alif_camera_channel_t channel,
                                                 const uint16_t *table, uint16_t len);

/**
 * @brief Load the lens-shading-correction LUT (Mali-C55 "MESH").
 *
 * @par Supported silicon: alif:ensemble:e4, alif:ensemble:e6, alif:ensemble:e8
 *
 * The Mali-C55 applies a 2D vignetting-correction surface stored
 * as a flattened uint16_t grid; each entry is a Q4.12 gain
 * applied to the matching grid cell.  Reference-not-copy.
 *
 * @param camera  Handle from @ref alp_camera_open.
 * @param lut     Pointer to the flattened LUT.  Non-NULL.
 * @param len     Total number of grid cells; 64..4096 inclusive.
 *
 * @return @ref ALP_OK / @ref ALP_ERR_INVAL /
 *         @ref ALP_ERR_NOT_PRESENT_ON_THIS_SOC /
 *         @ref ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_alif_camera_isp_lsc_lut_load(alp_camera_t *camera, const uint16_t *lut,
                                              uint16_t len);

#ifdef __cplusplus
}
#endif

#endif /* ALP_EXT_ALIF_CAMERA_H */
