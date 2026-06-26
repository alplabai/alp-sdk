/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wm8960.h
 * @brief Cirrus / Wolfson WM8960 stereo audio codec (I²C config + I²S data).
 *
 * Classic Wolfson stereo codec (RPi audio HAT standard).  This
 * driver covers the I²C config surface only -- chip probe + register
 * R/W.  Audio sample flow goes through `<alp/i2s.h>` once the
 * Wolfson init sequence is captured in a follow-up.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes
 *   NULL-arg smokes; no HiL silicon bring-up yet.  Treat all numbers
 *   + lifecycle sequencing as paper-correct only until the v1.0
 *   verification sweep lands.
 *
 * Datasheet: Cirrus / Wolfson WM8960 (Rev 4.5, Aug 2012).
 */

#ifndef ALP_CHIPS_WM8960_H
#define ALP_CHIPS_WM8960_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Fixed 7-bit I²C control address of the WM8960 (not re-addressable). */
#define WM8960_I2C_ADDR 0x1Au

/**
 * @brief Driver context for one WM8960 instance.
 *
 * Caller-allocated and owned; bound by @ref wm8960_init and torn down by
 * @ref wm8960_deinit.  Covers the I²C control surface only — audio sample
 * flow goes through the I²S path.  Not thread-safe — serialise calls that
 * share a context (and the underlying I²C bus) externally.
 */
typedef struct {
	alp_i2c_t *bus;         /**< Caller-opened I²C bus; borrowed, not owned/closed here. */
	uint8_t    addr;        /**< 7-bit control address bound at init. */
	bool       initialised; /**< True once @ref wm8960_init has bound the context. */
} wm8960_t;

/**
 * @brief Bind a context to a caller-opened I²C bus.
 *
 * @param[out] dev       Caller-allocated context to initialise.
 * @param[in]  bus       Opened I²C bus; borrowed for the context lifetime.
 * @param[in]  i2c_addr  7-bit address (always @ref WM8960_I2C_ADDR).
 * @return ALP_OK on success, or an ALP_ERR_* code on NULL args or I²C failure.
 */
alp_status_t wm8960_init(wm8960_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Write a 9-bit value into the register at @p reg (7-bit address).
 *
 * WM8960 uses a packed-on-the-wire format: top 7 bits of the first
 * byte are the register address, low bit + 8 bits of next byte are
 * the value.  The chip is write-only over I²C (registers cannot be read back).
 *
 * @param[in] dev       Context bound by @ref wm8960_init.
 * @param[in] reg       7-bit register address (0x00..0x37).
 * @param[in] val_9bit  Value to write; only the low 9 bits are significant.
 * @return ALP_OK on success, or an ALP_ERR_* code on NULL args or I²C failure.
 */
alp_status_t wm8960_write_reg(wm8960_t *dev, uint8_t reg, uint16_t val_9bit);

/**
 * @brief Issue a software reset (write to RESET register 0x0F).
 *
 * Returns all registers to their power-on defaults.
 *
 * @param[in] dev  Context bound by @ref wm8960_init.
 * @return ALP_OK on success, or an ALP_ERR_* code on NULL args or I²C failure.
 */
alp_status_t wm8960_soft_reset(wm8960_t *dev);

/**
 * @brief Release the driver context.  Does not close the borrowed I²C bus.
 *
 * @param[in] dev  Context to tear down; NULL is a no-op.
 */
void wm8960_deinit(wm8960_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_WM8960_H */
