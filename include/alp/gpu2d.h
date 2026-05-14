/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file gpu2d.h
 * @brief ALP SDK 2D graphics accelerator (D/AVE 2D class) abstraction.
 *
 * Portable 2D blit / fill / blend operations against framebuffer
 * surfaces.  Maps to the Alif Ensemble's D/AVE 2D ("GPU2D") block on
 * AEN-family SoMs; planned implementations land for the NXP i.MX 93
 * Vivante GC328 (when the vendor HAL ships) and as a software
 * fallback for SoMs without a 2D accelerator.
 *
 * Surface rationale: the Alif Ensemble audit
 * (internal AEN feature audit, top-five NEEDS-PORTABLE-
 * SURFACE gap) flagged GPU2D as the highest-demand block missing a
 * Zephyr driver class.  Customers migrating from V2N to AEN
 * silently lose the 2D acceleration if the SDK does not expose a
 * portable surface.  This header declares the API so customer code
 * compiles against every SoM; the actual GPU2D dispatch lands once
 * the AEN HAL pack stabilises (see roadmap in
 * the internal AEN feature audit §5.2).
 *
 * Backends:
 *   - AEN-family : Alif `alif_dave2d-driver` (vendor HAL).
 *   - i.MX 93    : Vivante GC328 (vendor BSP, planned).
 *   - V2N        : NOSUPPORT (no on-die 2D block).
 *   - Yocto      : DRM / KMS planes where available; NOSUPPORT
 *                  otherwise.
 *   - Baremetal  : NOSUPPORT.
 *
 * Concurrency: the singleton handle returned by @ref alp_gpu2d_open
 * is reentrant under a shared driver mutex.  Callers must serialise
 * @ref alp_gpu2d_fill_rect / @ref alp_gpu2d_blit / @ref alp_gpu2d_blend
 * issuance if the underlying HAL doesn't queue ops -- the v0.5 stub
 * doesn't perform any work so concurrency is not yet load-bearing.
 *
 * Typical usage:
 * @code
 *     alp_gpu2d_t *g = alp_gpu2d_open();
 *     const alp_gpu2d_surface_t fb = {
 *         .base         = framebuffer,
 *         .width        = 800,
 *         .height       = 480,
 *         .stride_bytes = 800 * 4,
 *         .format       = ALP_GPU2D_FMT_ARGB8888,
 *     };
 *     alp_gpu2d_fill_rect(g, &fb, 10, 10, 100, 50, 0xFFFF0000);  // red
 *     alp_gpu2d_close(g);
 * @endcode
 */

#ifndef ALP_GPU2D_H
#define ALP_GPU2D_H

#include <stdint.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Pixel format for 2D surfaces.  Backends honour the subset their
 *  HW supports; unsupported formats return ALP_ERR_NOSUPPORT.
 *  Field-level meanings:
 *   - ARGB8888: 32-bit, A:R:G:B = 8:8:8:8 (default for DAVE2D).
 *   - RGB565: 16-bit, R:G:B = 5:6:5.
 *   - A8: 8-bit alpha only (useful for masks).
 *   - RGB888: 24-bit packed; backend-defined byte order.
 *   - RGBA8888: 32-bit, R:G:B:A = 8:8:8:8 (alternate ordering). */
typedef enum {
    ALP_GPU2D_FMT_ARGB8888 = 0,
    ALP_GPU2D_FMT_RGB565   = 1,
    ALP_GPU2D_FMT_A8       = 2,
    ALP_GPU2D_FMT_RGB888   = 3,
    ALP_GPU2D_FMT_RGBA8888 = 4,
} alp_gpu2d_format_t;

/** Blend mode for @ref alp_gpu2d_blend.  Field-level meanings:
 *   - REPLACE: dst = src (no blend).
 *   - SRC_OVER: dst = src + dst*(1-src.a) (Porter-Duff).
 *   - ADDITIVE: dst = src + dst (clamped).
 *   - MULTIPLY: dst = src * dst. */
typedef enum {
    ALP_GPU2D_BLEND_REPLACE  = 0,
    ALP_GPU2D_BLEND_SRC_OVER = 1,
    ALP_GPU2D_BLEND_ADDITIVE = 2,
    ALP_GPU2D_BLEND_MULTIPLY = 3,
} alp_gpu2d_blend_mode_t;

/** Surface descriptor.  Lives in caller memory; copied internally
 *  per operation so the caller can reuse the struct or modify it
 *  between calls.  Field-level meanings:
 *   - base: pointer to the top-left pixel (no header byte).
 *   - width / height: dimensions in pixels.
 *   - stride_bytes: bytes from one row's start to the next
 *     (= width * bytes-per-pixel for tightly-packed surfaces).
 *   - format: one of @ref alp_gpu2d_format_t. */
typedef struct {
    void              *base;
    uint32_t           width;
    uint32_t           height;
    uint32_t           stride_bytes;
    alp_gpu2d_format_t format;
} alp_gpu2d_surface_t;

/** Opaque GPU2D handle.  Allocate via @ref alp_gpu2d_open. */
typedef struct alp_gpu2d alp_gpu2d_t;

/**
 * @brief Acquire the system-wide GPU2D accelerator handle.
 *
 * The handle is single-instance per process (mirrors how DAVE2D
 * exposes itself).  Subsequent open() calls return the same
 * underlying handle.  Backends without a 2D accelerator return
 * NULL with @ref alp_last_error = @ref ALP_ERR_NOSUPPORT.
 *
 * @return Handle on success, NULL with alp_last_error set on
 *         failure.
 */
alp_gpu2d_t *alp_gpu2d_open(void);

/**
 * @brief Fill an axis-aligned rectangle with a solid colour.
 *
 * The colour is interpreted per @p dst->format: ARGB8888 takes
 * the 32-bit value as-is, RGB565 ignores the high 16 bits, A8
 * takes only the high byte (alpha).
 *
 * @param[in] handle      Handle from @ref alp_gpu2d_open.
 * @param[in] dst         Destination surface.
 * @param[in] x, y        Top-left corner of the rect (pixels).
 * @param[in] w, h        Rect size (pixels).  Clipped to
 *                        @p dst's dimensions.
 * @param[in] argb_color  Colour in ARGB8888 (backend converts).
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL /
 *         ALP_ERR_OUT_OF_RANGE / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_gpu2d_fill_rect(alp_gpu2d_t *handle, const alp_gpu2d_surface_t *dst, uint32_t x,
                                 uint32_t y, uint32_t w, uint32_t h, uint32_t argb_color);

/**
 * @brief Copy a sub-rect from @p src into @p dst (no blend).
 *
 * Source + destination may overlap on the same buffer; backends
 * handle the safe direction internally.  Format conversion is
 * performed if @p src->format != @p dst->format and both are
 * in the backend's supported set; otherwise the call returns
 * @ref ALP_ERR_NOSUPPORT.
 *
 * @param[in] handle  Handle from @ref alp_gpu2d_open.
 * @param[in] src     Source surface.
 * @param[in] sx, sy  Top-left in @p src.
 * @param[in] dst     Destination surface.
 * @param[in] dx, dy  Top-left in @p dst.
 * @param[in] w, h    Rect size (pixels).
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL /
 *         ALP_ERR_OUT_OF_RANGE / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_gpu2d_blit(alp_gpu2d_t *handle, const alp_gpu2d_surface_t *src, uint32_t sx,
                            uint32_t sy, const alp_gpu2d_surface_t *dst, uint32_t dx, uint32_t dy,
                            uint32_t w, uint32_t h);

/**
 * @brief Alpha-blend @p src onto @p dst using the chosen mode.
 *
 * The blend mode determines how src + dst combine per-pixel:
 * see @ref alp_gpu2d_blend_mode_t for the formulas.
 *
 * @param[in] handle  Handle from @ref alp_gpu2d_open.
 * @param[in] src     Source surface.
 * @param[in] sx, sy  Top-left in @p src.
 * @param[in] dst     Destination surface.
 * @param[in] dx, dy  Top-left in @p dst.
 * @param[in] w, h    Rect size (pixels).
 * @param[in] mode    One of @ref alp_gpu2d_blend_mode_t.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL /
 *         ALP_ERR_OUT_OF_RANGE / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_gpu2d_blend(alp_gpu2d_t *handle, const alp_gpu2d_surface_t *src, uint32_t sx,
                             uint32_t sy, const alp_gpu2d_surface_t *dst, uint32_t dx, uint32_t dy,
                             uint32_t w, uint32_t h, alp_gpu2d_blend_mode_t mode);

/**
 * @brief Release the GPU2D handle.  NULL is a no-op.
 *
 * @param[in] handle  Handle from @ref alp_gpu2d_open, or NULL.
 */
void alp_gpu2d_close(alp_gpu2d_t *handle);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_GPU2D_H */
