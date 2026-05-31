/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lps22hb.h
 * @brief STMicroelectronics LPS22HB pressure sensor (I²C).
 *
 * 24-bit pressure + temperature MEMS barometer, ±0.1 hPa absolute
 * accuracy.  Common in ST's IoT eval boards alongside the
 * `lis2dw12` accelerometer already shipped.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: ST LPS22HB Rev 6 (Mar 2018).
 */

#ifndef ALP_CHIPS_LPS22HB_H
#define ALP_CHIPS_LPS22HB_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LPS22HB_I2C_ADDR_LOW  0x5Cu /**< SA0 = low. */
#define LPS22HB_I2C_ADDR_HIGH 0x5Du /**< SA0 = high (default). */

#define LPS22HB_REG_WHO_AM_I 0x0Fu
#define LPS22HB_WHO_AM_I     0xB1u

typedef struct {
    alp_i2c_t *bus;
    uint8_t    addr;
    bool       initialised;
} lps22hb_t;

/** @brief Bind context and verify WHO_AM_I = 0xB1. */
alp_status_t lps22hb_init(lps22hb_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** @brief Read WHO_AM_I register. */
alp_status_t lps22hb_read_id(lps22hb_t *dev, uint8_t *id_out);

/** @brief Soft reset (CTRL_REG2 bit 2 = SWRESET). */
alp_status_t lps22hb_soft_reset(lps22hb_t *dev);

/** @brief Release driver context. */
void lps22hb_deinit(lps22hb_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_LPS22HB_H */
