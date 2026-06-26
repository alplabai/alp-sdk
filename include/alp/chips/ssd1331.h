/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ssd1331.h
 * @brief Solomon Systech SSD1331 96 × 64 colour OLED driver (SPI).
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Public surface consumed by alp-studio block `blk_oled_ssd1331`.
 * Symbols carry the chip's natural prefix `ssd1331_*` — no `alp_`.
 *
 * v0.2 scope: SPI 4-wire (D/C# pin), 96 × 64 fixed geometry, RGB565
 * pixel format, caller-supplied framebuffer (12 KiB at full size — too
 * large to embed in the driver context safely on M-class targets).
 *
 * Datasheet: Solomon Systech SSD1331 v1.2 (2008).
 */

#ifndef ALP_CHIPS_SSD1331_H
#define ALP_CHIPS_SSD1331_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Fixed panel geometry.  SSD1331 is exclusively 96 × 64. */
#define SSD1331_WIDTH  96
#define SSD1331_HEIGHT 64

/** Bytes per pixel in the framebuffer (RGB565 native). */
#define SSD1331_BPP 2

/** Required framebuffer size in bytes. */
#define SSD1331_FB_BYTES (SSD1331_WIDTH * SSD1331_HEIGHT * SSD1331_BPP)

/**
 * Pack 5/6/5-bit RGB into a 16-bit native RGB565 word.
 *
 * The SSD1331 expects MSB-first when streamed over SPI; the impl handles
 * the byte order before pushing to the bus.
 */
static inline uint16_t ssd1331_rgb565(uint8_t r, uint8_t g, uint8_t b)
{
	return (uint16_t)(((r & 0xF8u) << 8) | ((g & 0xFCu) << 3) | (b >> 3));
}

/** Driver context.  Treat as opaque. */
typedef struct {
	alp_spi_t  *bus;         /**< SPI bus the panel hangs off (borrowed, not owned). */
	alp_gpio_t *dc;          /**< D/C# command-vs-data line. */
	uint8_t    *fb;          /**< Caller-supplied framebuffer of size @ref SSD1331_FB_BYTES. */
	size_t      fb_len;      /**< Length of @ref fb in bytes (≥ @ref SSD1331_FB_BYTES). */
	bool        initialised; /**< True once ssd1331_init() has succeeded. */
} ssd1331_t;

/**
 * @brief Initialise an SSD1331 over SPI.
 *
 * Drives the panel init sequence (display-off, remap+colour-depth,
 * mux ratio, contrast, master-current, …).  The caller supplies the
 * framebuffer (must be at least @ref SSD1331_FB_BYTES bytes); the
 * driver retains the pointer and writes into it via
 * @ref ssd1331_draw_pixel.
 *
 * @param dev    SSD1331 driver context (output; populated on success).
 * @param spi    Open SPI bus configured for the panel (MODE_0 typical,
 *               up to 6.25 MHz per the datasheet).
 * @param dc     GPIO open & configured as output for the D/C# line.
 * @param fb     Pointer to the caller's framebuffer storage.
 * @param fb_len Length of @p fb in bytes.  Must be ≥ @ref SSD1331_FB_BYTES.
 */
alp_status_t
ssd1331_init(ssd1331_t *dev, alp_spi_t *spi, alp_gpio_t *dc, uint8_t *fb, size_t fb_len);

/**
 * @brief Set the panel display ON or OFF.
 * @param dev Initialised driver context.
 * @param on  true = display on, false = display off.
 * @return `ALP_OK` on success, or an `alp_status_t` error on bus failure.
 */
alp_status_t ssd1331_set_display_on(ssd1331_t *dev, bool on);

/**
 * @brief Set master contrast attenuation.
 *
 * @param dev     Initialised driver context.
 * @param current Attenuation level 0..15 (datasheet command 0x87, 4-bit field).
 * @return `ALP_OK` on success, or an `alp_status_t` error on bus failure.
 */
alp_status_t ssd1331_set_master_current(ssd1331_t *dev, uint8_t current);

/**
 * @brief Wipe the in-memory framebuffer to black.  Does not push to the panel.
 * @param dev Initialised driver context.
 */
void ssd1331_clear(ssd1331_t *dev);

/**
 * @brief Set a single pixel in the framebuffer.
 *
 * Out-of-range coordinates are silently ignored (standard graphics-lib
 * contract — keeps callers from having to clip every primitive).
 *
 * @param dev    SSD1331 driver context (must be initialised first).
 * @param x      Column coordinate, 0..@ref SSD1331_WIDTH - 1.
 * @param y      Row coordinate, 0..@ref SSD1331_HEIGHT - 1.
 * @param colour RGB565 pixel value.  See the ssd1331_rgb565 helper.
 */
void ssd1331_draw_pixel(ssd1331_t *dev, uint16_t x, uint16_t y, uint16_t colour);

/**
 * @brief Push the entire in-memory framebuffer to the panel.
 * @param dev Initialised driver context.
 * @return `ALP_OK` on success, or an `alp_status_t` error on bus failure.
 */
alp_status_t ssd1331_display(ssd1331_t *dev);

/**
 * @brief Release the driver context.  Does not turn off the panel.
 * @param dev Driver context to release.
 */
void ssd1331_deinit(ssd1331_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_SSD1331_H */
