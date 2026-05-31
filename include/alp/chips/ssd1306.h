/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ssd1306.h
 * @brief Solomon Systech SSD1306 monochrome OLED driver.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Public surface consumed by alp-studio block `blk_oled_ssd1306`.
 * Symbols carry the chip's natural prefix `ssd1306_*` — no `alp_`.
 *
 * v0.1: I2C only, 128 × 64 default geometry, vertical-byte
 * framebuffer (1 bpp).  Pixel/clear/display API.  Font rendering
 * is intentionally out of scope — the studio block stacks LVGL
 * (or a project-supplied font) on top of `ssd1306_draw_pixel`.
 *
 * Datasheet: Solomon Systech SSD1306 v1.1 (2008).
 */

#ifndef ALP_CHIPS_SSD1306_H
#define ALP_CHIPS_SSD1306_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default 7-bit I2C addresses (D/C# pin selects). */
#define SSD1306_I2C_ADDR_LOW   0x3C
#define SSD1306_I2C_ADDR_HIGH  0x3D

/** Maximum supported geometry in v0.1.  128 × 64 is the dev-kit standard. */
#define SSD1306_MAX_WIDTH   128
#define SSD1306_MAX_HEIGHT  64
#define SSD1306_MAX_FB_BYTES (SSD1306_MAX_WIDTH * SSD1306_MAX_HEIGHT / 8)

/** Driver context.  Treat as opaque. */
typedef struct {
    alp_i2c_t *bus;
    uint8_t    addr;       /**< 7-bit I2C address. */
    uint16_t   width;
    uint16_t   height;
    bool       initialised;

    /** Vertical-byte framebuffer (one byte per 8 vertical pixels).
     *  Layout: page-major, column-minor, matching SSD1306 GDDRAM. */
    uint8_t    fb[SSD1306_MAX_FB_BYTES];
} ssd1306_t;

/**
 * @brief Initialise an SSD1306 over I2C.
 *
 * Runs the standard panel init sequence (charge pump, common output,
 * page mode, contrast).  Caller-provided geometry must be one of:
 *   - 128 × 64 (typical 0.96" / 1.3" panels)
 *   - 128 × 32 (slim panels)
 *
 * On success the framebuffer is cleared but the panel display state
 * is left to the caller — call `ssd1306_display(dev)` after drawing
 * to flush.
 */
alp_status_t ssd1306_init(ssd1306_t *dev,
                          alp_i2c_t *bus,
                          uint8_t i2c_addr,
                          uint16_t width,
                          uint16_t height);

/** Set the panel display ON or OFF (charge pump stays running). */
alp_status_t ssd1306_set_display_on(ssd1306_t *dev, bool on);

/** 0 = darkest, 255 = brightest.  Datasheet reset value is 0x7F. */
alp_status_t ssd1306_set_contrast(ssd1306_t *dev, uint8_t contrast);

/** Toggle inverted-display mode (every pixel value flipped). */
alp_status_t ssd1306_set_inverted(ssd1306_t *dev, bool inverted);

/** Wipe the in-memory framebuffer.  Does not push to the panel. */
void         ssd1306_clear(ssd1306_t *dev);

/**
 * @brief Set a single pixel in the framebuffer.
 *
 * Out-of-range coordinates are silently ignored — this is the
 * standard graphics-library contract and avoids forcing every
 * caller to clip.
 */
void         ssd1306_draw_pixel(ssd1306_t *dev,
                                uint16_t x, uint16_t y,
                                bool on);

/** Push the entire in-memory framebuffer to the panel. */
alp_status_t ssd1306_display(ssd1306_t *dev);

/** Release the driver context.  Does not turn off the panel. */
void         ssd1306_deinit(ssd1306_t *dev);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_CHIPS_SSD1306_H */
