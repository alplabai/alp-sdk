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

typedef enum {
    ALP_PIXFMT_MONO_VLSB = 0,   /**< 1bpp, vertical bytes (SSD1306 native). */
    ALP_PIXFMT_RGB565    = 1,
    ALP_PIXFMT_RGB888    = 2,
    ALP_PIXFMT_ARGB8888  = 3
} alp_pixfmt_t;

typedef struct {
    uint32_t display_id;    /**< Studio-resolved display instance. */
} alp_display_config_t;

typedef struct {
    uint16_t width;
    uint16_t height;
    alp_pixfmt_t format;
} alp_display_caps_t;

alp_display_t *alp_display_open(const alp_display_config_t *cfg);

alp_status_t alp_display_get_caps(alp_display_t *d, alp_display_caps_t *out);

/** Push a rectangular framebuffer region.  `pixels` size is implied by w*h*format. */
alp_status_t alp_display_blit(alp_display_t *d,
                              uint16_t x, uint16_t y,
                              uint16_t w, uint16_t h,
                              const void *pixels);

alp_status_t alp_display_clear(alp_display_t *d);

void alp_display_close(alp_display_t *d);

#ifdef __cplusplus
}
#endif

#endif  /* ALP_DISPLAY_H */
