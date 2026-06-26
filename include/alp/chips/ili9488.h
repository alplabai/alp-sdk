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
 * @par Driver status: [stub-impl]
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

#define ILI9488_WIDTH  320 /**< Native panel width in pixels (default orientation). */
#define ILI9488_HEIGHT 480 /**< Native panel height in pixels (default orientation). */

/** @brief Driver context.  Caller-allocated; treat fields as opaque. */
typedef struct {
	alp_spi_t  *bus;         /**< Bound SPI bus (borrowed; not owned/closed by driver). */
	alp_gpio_t *dc;          /**< D/C# select: low = command, high = data. */
	alp_gpio_t *reset;       /**< Active-low hardware RESET line. */
	bool        initialised; /**< True once ili9488_init() has run. */
} ili9488_t;

/**
 * @brief Initialise an ILI9488 over SPI.
 *
 * Binds the bus and control lines and runs the panel init sequence in 16 bpp
 * RGB565 mode.  All handles are borrowed and must outlive @p dev.
 *
 * @param dev    Caller-allocated context to initialise.
 * @param spi    Open SPI bus.
 * @param dc     D/C# command/data select GPIO.
 * @param reset  Active-low RESET GPIO.
 * @return ALP_OK on success; ALP_ERR_IO on bus/GPIO failure.
 */
alp_status_t ili9488_init(ili9488_t *dev, alp_spi_t *spi, alp_gpio_t *dc, alp_gpio_t *reset);

/**
 * @brief Set the active drawing window (inclusive coordinates).
 *
 * Subsequent @ref ili9488_write_pixels calls fill this rectangle.
 *
 * @param dev  Initialised context.
 * @param x0   Left column (inclusive), 0..ILI9488_WIDTH-1.
 * @param y0   Top row (inclusive), 0..ILI9488_HEIGHT-1.
 * @param x1   Right column (inclusive), >= @p x0.
 * @param y1   Bottom row (inclusive), >= @p y0.
 * @return ALP_OK on success; ALP_ERR_IO on bus failure.
 */
alp_status_t ili9488_set_window(ili9488_t *dev, uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1);

/**
 * @brief Push a buffer of native big-endian RGB565 pixels into the window.
 *
 * @param dev      Initialised context.
 * @param pixels   Pixel bytes (2 per pixel, big-endian RGB565); borrowed.
 * @param n_bytes  Length of @p pixels in bytes.
 * @return ALP_OK on success; ALP_ERR_IO on bus failure.
 */
alp_status_t ili9488_write_pixels(ili9488_t *dev, const uint8_t *pixels, size_t n_bytes);

/**
 * @brief Release the driver context.
 *
 * Clears the initialised flag; does not power down the panel or close the
 * borrowed SPI/GPIO handles.
 *
 * @param dev  Context to release (may be NULL).
 */
void ili9488_deinit(ili9488_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_ILI9488_H */
