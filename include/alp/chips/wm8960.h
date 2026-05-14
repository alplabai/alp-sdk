/*
 * Copyright 2026 ALP Lab AB
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

#define WM8960_I2C_ADDR 0x1Au

typedef struct {
    alp_i2c_t *bus;
    uint8_t    addr;
    bool       initialised;
} wm8960_t;

/** @brief Bind context to caller-opened I²C bus. */
alp_status_t wm8960_init(wm8960_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Write a 9-bit value into the register at @p reg (7-bit address).
 *
 * WM8960 uses a packed-on-the-wire format: top 7 bits of the first
 * byte are the register address, low bit + 8 bits of next byte are
 * the value.
 */
alp_status_t wm8960_write_reg(wm8960_t *dev, uint8_t reg, uint16_t val_9bit);

/** @brief Issue a software reset (write to RESET register 0x0F). */
alp_status_t wm8960_soft_reset(wm8960_t *dev);

/** @brief Release driver context. */
void wm8960_deinit(wm8960_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_WM8960_H */
