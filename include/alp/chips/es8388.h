/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file es8388.h
 * @brief Everest Semi ES8388 stereo audio codec (I²C config + I²S data).
 *
 * China-domestic cost-sensitive stereo codec (dominant on ESP32
 * audio boards).  Same shape as `wm8960` -- I²C control surface
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

#define ES8388_I2C_ADDR_LOW  0x10u /**< 7-bit I2C address when CE/AD0 is low. */
#define ES8388_I2C_ADDR_HIGH 0x11u /**< 7-bit I2C address when CE/AD0 is high. */

/** @brief ES8388 codec control-side context. Audio data flows over `<alp/i2s.h>`. */
typedef struct {
	alp_i2c_t *bus;         /**< Borrowed I2C control bus (caller-owned, kept open). */
	uint8_t    addr;        /**< 7-bit I2C address (see ES8388_I2C_ADDR_*). */
	bool       initialised; /**< True once es8388_init() has bound the bus. */
} es8388_t;

/**
 * @brief Bind a context to a caller-opened I2C control bus.
 *
 * The @p bus handle is borrowed, not owned — keep it open for the context
 * lifetime. Does not reset or configure the codec.
 *
 * @param dev       Output: caller-allocated context to populate.
 * @param bus       Open I2C bus the codec's control port sits on.
 * @param i2c_addr  7-bit I2C address (see ES8388_I2C_ADDR_LOW / _HIGH).
 * @return `ALP_OK` on success; an `ALP_ERR_*` status otherwise.
 */
alp_status_t es8388_init(es8388_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read one 8-bit control register.
 *
 * @param dev  Initialised context.
 * @param reg  Register index to read.
 * @param val  Output: receives the register value.
 * @return `ALP_OK` on success; an `ALP_ERR_*` status on a bus error.
 */
alp_status_t es8388_read_reg(es8388_t *dev, uint8_t reg, uint8_t *val);

/**
 * @brief Write one 8-bit control register.
 *
 * @param dev  Initialised context.
 * @param reg  Register index to write.
 * @param val  Value to write.
 * @return `ALP_OK` on success; an `ALP_ERR_*` status on a bus error.
 */
alp_status_t es8388_write_reg(es8388_t *dev, uint8_t reg, uint8_t val);

/**
 * @brief Issue a software reset (writes RESET_CONTROL = 0x80).
 *
 * @param dev  Initialised context.
 * @return `ALP_OK` on success; an `ALP_ERR_*` status on a bus error.
 */
alp_status_t es8388_soft_reset(es8388_t *dev);

/**
 * @brief Release the driver context.
 *
 * Clears the bound bus reference but does not close it — the caller owns the
 * I2C handle.
 *
 * @param dev  Context to release.
 */
void es8388_deinit(es8388_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_ES8388_H */
