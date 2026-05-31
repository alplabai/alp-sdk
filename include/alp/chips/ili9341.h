/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ili9341.h
 * @brief Ilitek ILI9341 240 × 320 TFT driver (SPI).
 *
 * Most-common embedded TFT controller.  4-wire SPI plus D/C# command/
 * data select.  Same shape as `st7789` — different init sequence.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl] — init + set-window + write-pixels.
 *
 * Datasheet: Ilitek ILI9341 v1.11 (Jun 2011).
 */

#ifndef ALP_CHIPS_ILI9341_H
#define ALP_CHIPS_ILI9341_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ILI9341_WIDTH  240
#define ILI9341_HEIGHT 320

typedef struct {
    alp_spi_t  *bus;
    alp_gpio_t *dc;
    alp_gpio_t *reset;
    bool        initialised;
} ili9341_t;

/** @brief Initialise an ILI9341 over SPI.  See @ref st7789_init for shape. */
alp_status_t ili9341_init(ili9341_t  *dev,
                          alp_spi_t  *spi,
                          alp_gpio_t *dc,
                          alp_gpio_t *reset);

/** @brief Set the active drawing window (inclusive coords). */
alp_status_t ili9341_set_window(ili9341_t *dev,
                                uint16_t   x0,
                                uint16_t   y0,
                                uint16_t   x1,
                                uint16_t   y1);

/** @brief Push a buffer of native big-endian RGB565 pixels. */
alp_status_t ili9341_write_pixels(ili9341_t     *dev,
                                  const uint8_t *pixels,
                                  size_t         n_bytes);

/** @brief Release the driver context. */
void ili9341_deinit(ili9341_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_ILI9341_H */
