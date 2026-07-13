/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file gpu2d.h
 * @brief Alp SDK 2D graphics accelerator (D/AVE 2D class) abstraction.
 *
 * Portable 2D blit / fill / blend operations against framebuffer
 * surfaces.  Maps to the Alif Ensemble's D/AVE 2D ("GPU2D") block on
 * AEN-family SoMs; every other SoM is served by a portable software
 * fallback that does the same fill / blit / blend on the CPU.
 *
 * Surface rationale: AEN-family SoMs expose a hardware 2D accelerator,
 * while Zephyr has no portable 2D accelerator driver class.  Customers
 * migrating from V2N to AEN silently lose the acceleration if the SDK
 * does not expose a portable surface.  This header declares the API so
 * customer code compiles against every SoM.
 *
 * Backends:
 *   - AEN-family : D/AVE 2D hardware block (SDK backend; no vendor
 *                  SDK dependency in app code).  Bench-unverified
 *                  today -- see src/backends/gpu2d/alif_dave2d.c.
 *   - all others : portable software fallback (CPU fill/blit/blend;
 *                  src/backends/gpu2d/sw_fallback.c).  This is what
 *                  V2N, i.MX 93 (whose 2D engine is PXP, not a
 *                  GPU2D peer -- see ADR 0008), and ALP_OS=yocto
 *                  Linux builds use.  Plain-CMake bare-metal builds
 *                  still link the NOSUPPORT stub (no backend
 *                  registry there yet).
 *
 * Concurrency: the singleton handle returned by @ref alp_gpu2d_open
 * is reentrant under a shared driver mutex.  Callers must serialise
 * @ref alp_gpu2d_fill_rect / @ref alp_gpu2d_blit / @ref alp_gpu2d_blend
 * issuance -- the software fallback writes caller memory directly and
 * the D/AVE 2D HAL's display-list builder is not itself thread-safe.
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
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.5 new -- AEN audit headline gap.  Portable surface but only one silicon family populates it today.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_GPU2D_H
#define ALP_GPU2D_H

#include <stdint.h>

#include "alp/cap_instance.h"
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

/** Blend mode for @ref alp_gpu2d_blend.  Colours are straight
 *  (non-premultiplied) alpha.  Field-level meanings:
 *   - REPLACE: dst = src (no blend).
 *   - SRC_OVER: dst = src*src.a + dst*(1-src.a) (straight-alpha
 *     src-over: a transparent src leaves dst untouched, an opaque
 *     src replaces it).
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
 * @brief Acquire a GPU2D handle.
 *
 * With the portable software fallback enabled (the default,
 * CONFIG_ALP_SDK_GPU2D_SW_FALLBACK) open() succeeds on every SoM --
 * SoMs without a 2D accelerator are served by the CPU path, so
 * there is no NOSUPPORT case at open time.  NULL is returned only
 * when no backend is compiled in at all (@ref alp_last_error =
 * ALP_ERR_NOT_PRESENT_ON_THIS_SOC) or when the handle pool is
 * exhausted (ALP_ERR_NOMEM).
 *
 * The pool defaults to ONE handle (the 2D engine is a system-wide
 * singleton): a second open() before @ref alp_gpu2d_close fails
 * with ALP_ERR_NOMEM rather than aliasing the live handle.  The
 * pool size is the CONFIG_ALP_SDK_MAX_GPU2D_HANDLES compile-time
 * override (no Kconfig entry today -- define it on the build).
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
alp_status_t alp_gpu2d_fill_rect(alp_gpu2d_t               *handle,
                                 const alp_gpu2d_surface_t *dst,
                                 uint32_t                   x,
                                 uint32_t                   y,
                                 uint32_t                   w,
                                 uint32_t                   h,
                                 uint32_t                   argb_color);

/**
 * @brief Copy a sub-rect from @p src into @p dst (no blend).
 *
 * Overlap contract: pixels are processed top-to-bottom,
 * left-to-right, so an overlapping same-buffer copy toward EARLIER
 * memory (dy < sy, or dy == sy with dx <= sx) is safe; a copy
 * toward later memory may read pixels the op already wrote
 * (backends do not reverse-iterate).  Callers needing overlap-safe
 * moves in both directions must use distinct buffers.  Format
 * conversion is performed if @p src->format != @p dst->format and
 * both are in the backend's supported set; otherwise the call
 * returns @ref ALP_ERR_NOSUPPORT.
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
alp_status_t alp_gpu2d_blit(alp_gpu2d_t               *handle,
                            const alp_gpu2d_surface_t *src,
                            uint32_t                   sx,
                            uint32_t                   sy,
                            const alp_gpu2d_surface_t *dst,
                            uint32_t                   dx,
                            uint32_t                   dy,
                            uint32_t                   w,
                            uint32_t                   h);

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
alp_status_t alp_gpu2d_blend(alp_gpu2d_t               *handle,
                             const alp_gpu2d_surface_t *src,
                             uint32_t                   sx,
                             uint32_t                   sy,
                             const alp_gpu2d_surface_t *dst,
                             uint32_t                   dx,
                             uint32_t                   dy,
                             uint32_t                   w,
                             uint32_t                   h,
                             alp_gpu2d_blend_mode_t     mode);

/**
 * @brief Release the GPU2D handle.  NULL is a no-op.
 *
 * @param[in] handle  Handle from @ref alp_gpu2d_open, or NULL.
 */
void alp_gpu2d_close(alp_gpu2d_t *handle);

/**
 * @brief Query the capabilities of an opened GPU2D handle.
 *
 * @param handle  Handle from @ref alp_gpu2d_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p handle is NULL.
 */
const alp_capabilities_t *alp_gpu2d_capabilities(const alp_gpu2d_t *handle);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_GPU2D_H */
