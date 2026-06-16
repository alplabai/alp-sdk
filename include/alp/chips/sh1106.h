/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file sh1106.h
 * @brief Sino Wealth SH1106 128 × 64 monochrome OLED driver (I²C).
 *
 * Drop-in alternative to SSD1306 for 1.3" OLED panels.  Key
 * difference vs. SSD1306: 132-column internal RAM offset (visible
 * 128 columns start at column 2), and page-mode-only addressing.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl] — init + clear + draw-pixel +
 *   display.  Geometry fixed at 128 × 64.
 *
 * Datasheet: Sino Wealth SH1106 v2.4 (Mar 2008).
 */

#ifndef ALP_CHIPS_SH1106_H
#define ALP_CHIPS_SH1106_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SH1106_I2C_ADDR_LOW 0x3Cu
#define SH1106_I2C_ADDR_HIGH 0x3Du

#define SH1106_WIDTH 128
#define SH1106_HEIGHT 64
#define SH1106_FB_BYTES (SH1106_WIDTH * SH1106_HEIGHT / 8)

typedef struct {
	alp_i2c_t *bus;
	uint8_t    addr;
	bool       initialised;
	uint8_t    fb[SH1106_FB_BYTES];
} sh1106_t;

/** @brief Initialise an SH1106 over I²C. */
alp_status_t sh1106_init(sh1106_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** @brief Set the panel display ON or OFF. */
alp_status_t sh1106_set_display_on(sh1106_t *dev, bool on);

/** @brief Wipe the in-memory framebuffer.  Does not push to panel. */
void sh1106_clear(sh1106_t *dev);

/** @brief Set a single framebuffer pixel.  OOB silently ignored. */
void sh1106_draw_pixel(sh1106_t *dev, uint16_t x, uint16_t y, bool on);

/** @brief Push the full framebuffer to the panel. */
alp_status_t sh1106_display(sh1106_t *dev);

/** @brief Release driver context. */
void sh1106_deinit(sh1106_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_SH1106_H */
