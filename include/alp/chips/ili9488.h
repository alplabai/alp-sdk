/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ili9488.h
 * @brief Ilitek ILI9488 320 × 480 TFT driver (SPI).
 *
 * Larger sibling of ILI9341.  4-wire SPI, default 18 bpp; this
 * driver runs it in 16 bpp RGB565 mode (3-byte SPI-pixel packing
 * is not portable enough across host SPI controllers).
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [partial-impl]
 *
 * Datasheet: Ilitek ILI9488 v1.00 (May 2012).
 */

#ifndef ALP_CHIPS_ILI9488_H
#define ALP_CHIPS_ILI9488_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ILI9488_WIDTH  320
#define ILI9488_HEIGHT 480

typedef struct {
	alp_spi_t  *bus;
	alp_gpio_t *dc;
	alp_gpio_t *reset;
	bool        initialised;
} ili9488_t;

/** @brief Initialise an ILI9488 over SPI. */
alp_status_t ili9488_init(ili9488_t *dev, alp_spi_t *spi, alp_gpio_t *dc, alp_gpio_t *reset);

/** @brief Set the active drawing window (inclusive). */
alp_status_t ili9488_set_window(ili9488_t *dev, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/** @brief Push a buffer of native big-endian RGB565 pixels. */
alp_status_t ili9488_write_pixels(ili9488_t *dev, const uint8_t *pixels, size_t n_bytes);

/** @brief Release the driver context. */
void ili9488_deinit(ili9488_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_ILI9488_H */
