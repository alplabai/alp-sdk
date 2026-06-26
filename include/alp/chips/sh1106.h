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

#define SH1106_I2C_ADDR_LOW  0x3Cu /**< 7-bit I2C address with SA0 strapped low. */
#define SH1106_I2C_ADDR_HIGH 0x3Du /**< 7-bit I2C address with SA0 strapped high. */

#define SH1106_WIDTH    128                                /**< Visible panel width, in pixels. */
#define SH1106_HEIGHT   64                                 /**< Visible panel height, in pixels. */
#define SH1106_FB_BYTES (SH1106_WIDTH * SH1106_HEIGHT / 8) /**< Framebuffer size, in bytes. */

/** @brief Driver context for one SH1106 OLED. Caller-allocated; populated by
 *  @ref sh1106_init. Holds the full off-screen framebuffer. Not thread-safe:
 *  serialise access to a single instance. */
typedef struct {
	alp_i2c_t *bus;                 /**< Caller-opened I2C bus (not owned). */
	uint8_t    addr;                /**< 7-bit I2C address in use. */
	bool       initialised;         /**< True once @ref sh1106_init succeeds. */
	uint8_t    fb[SH1106_FB_BYTES]; /**< Off-screen framebuffer; flushed by @ref sh1106_display. */
} sh1106_t;

/** @brief Initialise an SH1106 over I²C.
 *  @param dev      Driver context (output); caller-allocated.
 *  @param bus      Caller-opened I2C bus (not owned, must outlive @p dev).
 *  @param i2c_addr 7-bit address (@ref SH1106_I2C_ADDR_LOW or @ref SH1106_I2C_ADDR_HIGH).
 *  @return ALP_OK on success, ALP_ERR_INVAL on NULL args, ALP_ERR_IO on bus error. */
alp_status_t sh1106_init(sh1106_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** @brief Set the panel display ON or OFF.
 *  @param dev Initialised driver context.
 *  @param on  true to turn the display on, false to blank it.
 *  @return ALP_OK on success, ALP_ERR_IO on bus error. */
alp_status_t sh1106_set_display_on(sh1106_t *dev, bool on);

/** @brief Wipe the in-memory framebuffer.  Does not push to panel.
 *  @param dev Initialised driver context. */
void sh1106_clear(sh1106_t *dev);

/** @brief Set a single framebuffer pixel.  OOB silently ignored.
 *  @param dev Initialised driver context.
 *  @param x   Column, 0..@ref SH1106_WIDTH - 1.
 *  @param y   Row, 0..@ref SH1106_HEIGHT - 1.
 *  @param on  true lights the pixel, false clears it. */
void sh1106_draw_pixel(sh1106_t *dev, uint16_t x, uint16_t y, bool on);

/** @brief Push the full framebuffer to the panel.
 *  @param dev Initialised driver context.
 *  @return ALP_OK on success, ALP_ERR_IO on bus error. */
alp_status_t sh1106_display(sh1106_t *dev);

/** @brief Release driver context.  Idempotent.
 *  @param dev Driver context (may be NULL, in which case the call is a no-op). */
void sh1106_deinit(sh1106_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_SH1106_H */
