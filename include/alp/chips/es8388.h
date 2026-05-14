/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file es8388.h
 * @brief Everest Semi ES8388 stereo audio codec (I²C config + I²S data).
 *
 * China-domestic cost-sensitive stereo codec (dominant on ESP32
 * audio carriers).  Same shape as `wm8960` -- I²C control surface
 * here; audio sample path via `<alp/i2s.h>`.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes
 *   NULL-arg smokes; no HiL silicon bring-up yet.  Treat all numbers
 *   + lifecycle sequencing as paper-correct only until the v1.0
 *   verification sweep lands.
 *
 * Datasheet: Everest Semi ES8388 datasheet v6.5 (Mar 2010).
 */

#ifndef ALP_CHIPS_ES8388_H
#define ALP_CHIPS_ES8388_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ES8388_I2C_ADDR_LOW 0x10u
#define ES8388_I2C_ADDR_HIGH 0x11u

typedef struct {
    alp_i2c_t *bus;
    uint8_t    addr;
    bool       initialised;
} es8388_t;

/** @brief Bind context to caller-opened I²C bus. */
alp_status_t es8388_init(es8388_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** @brief Read register at @p reg. */
alp_status_t es8388_read_reg(es8388_t *dev, uint8_t reg, uint8_t *val);

/** @brief Write @p val to register @p reg. */
alp_status_t es8388_write_reg(es8388_t *dev, uint8_t reg, uint8_t val);

/** @brief Issue software reset (RESET_CONTROL = 0x80). */
alp_status_t es8388_soft_reset(es8388_t *dev);

/** @brief Release driver context. */
void es8388_deinit(es8388_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_ES8388_H */
