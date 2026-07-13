/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file camera.h
 * @brief Alp SDK camera abstraction (stub for v0.1).
 *
 * v0.2 ships a real MIPI CSI-2 wrapper for the V2N family.  v0.1
 * declares the surface so app code can compile against it; the
 * implementation returns ALP_ERR_NOSUPPORT on every backend.
 *

 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.5 added alp_camera_configure_isp -- surface tentative pending real hardware feedback.  Base capture path stable; ISP block experimental.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_CAMERA_H
#define ALP_CAMERA_H

#include <stdint.h>
#include "alp/cap_instance.h"
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct alp_camera alp_camera_t;

typedef struct {
	uint32_t     camera_id;
	uint16_t     width;
	uint16_t     height;
	uint8_t      fps;
	alp_pixfmt_t format;
} alp_camera_config_t;

/**
 * @brief Default-initialize an @ref alp_camera_config_t for camera @p id.
 *
 * Identity from @p id.  There is no universally-common sensor
 * resolution, so @c width / @c height default to 0 as an explicit
 * "you must choose" sentinel -- @ref alp_camera_open rejects an
 * out-of-range configuration, so a caller who forgets to set them
 * fails loudly rather than opening at an unintended size.  @c fps
 * defaults to 30 (the common video frame rate) and @c format defaults
 * to @ref ALP_PIXFMT_RGB565 (the widely-supported embedded-camera
 * default -- @ref ALP_PIXFMT_MONO_VLSB, the enum's zero value, is a
 * narrow SSD1306-specific format and would be a misleading default
 * here). Set @c width / @c height before calling open().
 *
 * @note Expands to a compound literal (a GCC/Clang extension in C++ -- the
 *       SDK's toolchains; standard through C23).  Usable as an initializer
 *       or an expression.  On a compiler that rejects compound literals in
 *       C++ (e.g. MSVC), initialize the config's fields individually.
 */
#define ALP_CAMERA_CONFIG_DEFAULT(id) \
	((alp_camera_config_t){ \
	    .camera_id = (id), .width = 0u, .height = 0u, .fps = 30u, .format = ALP_PIXFMT_RGB565 })

typedef struct {
	void    *data;
	size_t   size;
	uint64_t timestamp_us;
} alp_camera_frame_t;

/**
 * @brief Open a camera capture handle.
 *
 * @param[in] cfg  Capture configuration; @c camera_id selects the
 *                 device, width/height/fps/format request the stream
 *                 shape.  Must be non-NULL.
 *
 * @return Open handle on success, or NULL with @ref alp_last_error
 *         set to one of ALP_ERR_INVAL (NULL cfg or out-of-range
 *         field), ALP_ERR_NOT_READY (no backend wired),
 *         ALP_ERR_NOSUPPORT (backend lacks a camera class).
 */
alp_camera_t *alp_camera_open(const alp_camera_config_t *cfg);

/**
 * @brief Start streaming.
 *
 * Frames become available via @ref alp_camera_capture after this call
 * returns ALP_OK.  Idempotent if the stream is already running.
 *
 * @param[in] c  Handle from @ref alp_camera_open.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_NOSUPPORT / ALP_ERR_IO.
 */
alp_status_t alp_camera_start(alp_camera_t *c);

/**
 * @brief Stop streaming.
 *
 * Backend may keep the handle warm for a subsequent
 * @ref alp_camera_start.  In-flight frames captured via
 * @ref alp_camera_capture remain valid until released.
 *
 * @param[in] c  Handle from @ref alp_camera_open.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_NOSUPPORT / ALP_ERR_IO.
 */
alp_status_t alp_camera_stop(alp_camera_t *c);

/**
 * @brief Block until next frame is available.
 *
 * Caller does not own the frame buffer; release via
 * @ref alp_camera_release once the data is consumed.
 *
 * @param[in]  c           Handle from @ref alp_camera_open.
 * @param[out] out         Receives the frame descriptor (buffer
 *                         pointer + size + capture timestamp).
 *                         Must be non-NULL.
 * @param[in]  timeout_ms  Max wait in milliseconds; UINT32_MAX for
 *                         "wait indefinitely".
 *
 * @return ALP_OK / ALP_ERR_INVAL (NULL out or handle) /
 *         ALP_ERR_NOT_READY (stream not started) / ALP_ERR_TIMEOUT /
 *         ALP_ERR_NOSUPPORT / ALP_ERR_IO.
 */
alp_status_t alp_camera_capture(alp_camera_t *c, alp_camera_frame_t *out, uint32_t timeout_ms);

/**
 * @brief Return the frame buffer to the backend after consumption.
 *
 * After release the frame's @c data pointer is invalid -- backends
 * may immediately reuse the buffer for the next capture.
 *
 * @param[in] c      Handle from @ref alp_camera_open.
 * @param[in] frame  Frame descriptor previously filled by
 *                   @ref alp_camera_capture.  Must be non-NULL.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_camera_release(alp_camera_t *c, alp_camera_frame_t *frame);

/**
 * @brief Close the handle and release backend resources.  Idempotent.
 *
 * NULL is a no-op.  In-flight frames are implicitly released; their
 * @c data pointers become invalid immediately on return.
 *
 * @param[in] c  Handle from @ref alp_camera_open, or NULL.
 */
void alp_camera_close(alp_camera_t *c);

/**
 * @brief Query the capabilities of an opened camera handle.
 *
 * @param c  Handle from @ref alp_camera_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p c is NULL.
 */
const alp_capabilities_t *alp_camera_capabilities(const alp_camera_t *c);

/* ================================================================== */
/* ISP (Image Signal Processor) configuration                          */
/*                                                                     */
/* AEN-family E4 / E6 / E8 ship a dedicated ISP                       */
/* (Alif's hardened VeriSilicon ISP Pico (vsi,isp-pico) path) that    */
/* Zephyr's portable                                                  */
/* drivers/video/ class doesn't expose at the on-chip-ISP level --     */
/* the existing class covers sensor bridges + format negotiation but   */
/* not in-line image processing.  Customers migrating from V2N to     */
/* AEN otherwise silently lose ISP acceleration.                       */
/*                                                                     */
/* The minimal v0.5 surface declared below covers the headline ISP    */
/* primitives most cameras want enabled by default; finer-grained     */
/* tuning (per-channel gain tables, lens-shading-correction LUTs,     */
/* white-balance setpoints) can be added in follow-up commits as     */
/* customer demand surfaces.                                          */
/* ================================================================== */

/** Coarse ISP feature toggles.  All-zeros = bypass / passthrough --
 *  the sensor's raw frame reaches the application unchanged.  Each
 *  bit enables one major ISP pipeline stage.  Field-level meanings:
 *   - auto_exposure: AE convergence loop adjusts exposure +
 *     analog/digital gain.
 *   - auto_white_balance: AWB statistics drive the per-channel
 *     gain block.
 *   - auto_focus: drives the lens VCM (cameras with a focusable
 *     lens module).
 *   - lens_shading: applies the lens vignetting-correction LUT.
 *   - dead_pixel_correction: replaces flagged stuck pixels with
 *     neighbour-averaged values.
 *   - noise_reduction: 2D / 3D temporal NR (backend-defined). */
typedef struct {
	bool auto_exposure;
	bool auto_white_balance;
	bool auto_focus;
	bool lens_shading;
	bool dead_pixel_correction;
	bool noise_reduction;
	/** Picture-tuning offsets, -128..+127.  Applied after the
     *  auto-* feedback loops resolve to their setpoints.
     *  Field-level meanings:
     *   - brightness: pre-gamma luma offset.
     *   - contrast: luma scale around mid-grey.
     *   - saturation: chroma scale around grey (0 = monochrome). */
	int8_t  brightness;
	int8_t  contrast;
	int8_t  saturation;
	uint8_t reserved;
} alp_camera_isp_config_t;

/**
 * @brief Apply an ISP configuration to an open camera stream.
 *
 * Safe to call before or after @ref alp_camera_start; backends
 * latch the config and apply on the next frame boundary.
 * Backends without an on-die ISP return @ref ALP_ERR_NOSUPPORT.
 *
 * @param[in] camera  Handle from @ref alp_camera_open.
 * @param[in] isp     Configuration.  Must be non-NULL.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL /
 *         ALP_ERR_NOSUPPORT (backend lacks an ISP -- V2N today,
 *         AEN-family E3 / E5 / E7 silicon without the
 *         optional ISP fabric) / ALP_ERR_IO.
 */
alp_status_t alp_camera_configure_isp(alp_camera_t *camera, const alp_camera_isp_config_t *isp);

#ifdef __cplusplus
}
#endif

#endif /* ALP_CAMERA_H */
