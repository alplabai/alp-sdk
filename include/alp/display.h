/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file display.h
 * @brief ALP SDK display abstraction.
 *
 * v0.1: thin wrapper around the underlying OS display layer.  On
 * Zephyr this routes through Zephyr's `display_*` driver API so
 * generic SSD1306 / SSD1351 / similar parts work via devicetree.
 *
 * v0.2 will add:
 *   - alp_display_lvgl_attach() for LVGL flush integration.
 *   - DSI / parallel-RGB framebuffer paths for the V2N family.
 *

 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.3 placeholder; no real backend impl yet.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_DISPLAY_H
#define ALP_DISPLAY_H

#include <stdint.h>
#include <stddef.h>
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct alp_display alp_display_t;

/* alp_pixfmt_t is defined in <alp/peripheral.h> so it can be
 * shared with the camera surface without a forward dependency. */

typedef struct {
    uint32_t display_id;    /**< Studio-resolved display instance. */
} alp_display_config_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    alp_pixfmt_t format;
} alp_display_caps_t;

/**
 * @brief Open the display handle defined by @p cfg.
 *
 * @param[in] cfg  Display id.  Must be non-NULL.
 *
 * @return Open handle on success; NULL with @ref alp_last_error
 *         set to @ref ALP_ERR_INVAL / @ref ALP_ERR_NOT_READY
 *         (display id not DT-resolved) / @ref ALP_ERR_NOSUPPORT.
 */
alp_display_t *alp_display_open(const alp_display_config_t *cfg);

/**
 * @brief Read the display's static capabilities (geometry + pixel format).
 *
 * @param[in]  d    Handle from @ref alp_display_open.
 * @param[out] out  Filled with width / height / format.
 *                  Must be non-NULL.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_display_get_caps(alp_display_t *d, alp_display_caps_t *out);

/**
 * @brief Push a rectangular framebuffer region.  `pixels` size is implied by w*h*format.
 *
 * @param[in] d       Handle from @ref alp_display_open.
 * @param[in] x, y    Top-left of the destination rect (pixels).
 * @param[in] w, h    Size of the rect (pixels).
 * @param[in] pixels  Source framebuffer pointer; must be non-NULL.
 *                    Byte size = `w * h * bytes-per-pixel-for-format`.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_OUT_OF_RANGE
 *         (rect outside display caps) / ALP_ERR_NOT_READY /
 *         ALP_ERR_IO / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_display_blit(alp_display_t *d,
                              uint16_t x, uint16_t y,
                              uint16_t w, uint16_t h,
                              const void *pixels);

/**
 * @brief Clear the framebuffer to the background colour.
 *
 * @param[in] d  Handle from @ref alp_display_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY /
 *         ALP_ERR_IO / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_display_clear(alp_display_t *d);

/**
 * @brief Release the display handle.  Idempotent on NULL.
 *
 * @param[in] d  Handle from @ref alp_display_open, or NULL.
 */
void alp_display_close(alp_display_t *d);

#ifdef __cplusplus
}
#endif

#endif  /* ALP_DISPLAY_H */
