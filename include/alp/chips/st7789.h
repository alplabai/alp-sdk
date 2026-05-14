/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file st7789.h
 * @brief Sitronix ST7789V 240 × 240 / 240 × 320 IPS TFT driver (SPI).
 *
 * Workhorse 16-bit RGB565 TFT controller for round and rectangular
 * IPS panels.  4-wire SPI plus D/C# command/data select pin.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl] — init sequence + window-write +
 *   set-pixel-window only.  Full font/blit primitives lean on LVGL
 *   or the SDK's gfx_compat thin shim.
 *
 * Datasheet: Sitronix ST7789V Rev 1.4 (Mar 2017).
 */

#ifndef ALP_CHIPS_ST7789_H
#define ALP_CHIPS_ST7789_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ST7789_MAX_WIDTH  240
#define ST7789_MAX_HEIGHT 320

typedef struct {
    alp_spi_t  *bus;
    alp_gpio_t *dc;          /**< D/C# command-vs-data line. */
    alp_gpio_t *reset;       /**< Optional hardware reset; NULL = software-only. */
    uint16_t    width;
    uint16_t    height;
    bool        initialised;
} st7789_t;

/**
 * @brief Initialise an ST7789 over SPI.
 *
 * Runs reset, software-reset, sleep-out, colour-mode (16-bit RGB565),
 * memory-data-access-control, display-on.
 *
 * @param dev     Output: caller-allocated driver context.
 * @param spi     Open SPI bus.
 * @param dc      Open D/C# GPIO (active low = command).
 * @param reset   Optional reset GPIO; NULL = use software reset only.
 * @param width   Panel width in pixels (≤ 240).
 * @param height  Panel height in pixels (≤ 320).
 * @return `ALP_OK` on success.
 */
alp_status_t st7789_init(st7789_t   *dev,
                         alp_spi_t  *spi,
                         alp_gpio_t *dc,
                         alp_gpio_t *reset,
                         uint16_t    width,
                         uint16_t    height);

/**
 * @brief Set the active drawing window before pixel pushes.
 *
 * @param dev  Initialised driver context.
 * @param x0   Inclusive left edge.
 * @param y0   Inclusive top edge.
 * @param x1   Inclusive right edge (must be > x0).
 * @param y1   Inclusive bottom edge (must be > y0).
 * @return `ALP_OK` on success.
 */
alp_status_t st7789_set_window(st7789_t *dev,
                               uint16_t  x0,
                               uint16_t  y0,
                               uint16_t  x1,
                               uint16_t  y1);

/**
 * @brief Push a buffer of native-format RGB565 pixels.
 *
 * @param dev      Initialised driver context.
 * @param pixels   Caller-owned big-endian RGB565 pixel buffer.
 * @param n_bytes  Length in bytes (must be even).
 * @return `ALP_OK` on success.
 */
alp_status_t st7789_write_pixels(st7789_t      *dev,
                                 const uint8_t *pixels,
                                 size_t         n_bytes);

/** @brief Release the driver context.  Does not power down the panel. */
void st7789_deinit(st7789_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_ST7789_H */
