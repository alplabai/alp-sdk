/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file as5048a_b.h
 * @brief ams AS5048A / AS5048B 14-bit magnetic rotary encoder.
 *
 * AS5048A speaks SPI; AS5048B speaks I²C.  This driver covers the
 * I²C variant — AS5048B at 7-bit address 0x40 (strap-selectable
 * 0x40..0x43).  SPI variant lives in a parallel `as5048a_spi`
 * driver in a follow-up.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: ams AS5048A/B v1.7 (Nov 2018).
 */

#ifndef ALP_CHIPS_AS5048A_B_H
#define ALP_CHIPS_AS5048A_B_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AS5048B_I2C_ADDR_BASE 0x40u /**< OR with 0..3 (A1:A2 strap). */

#define AS5048B_REG_ANGLE_HI 0xFEu
#define AS5048B_REG_ANGLE_LO 0xFFu

typedef struct {
    alp_i2c_t *bus;
    uint8_t    addr;
    bool       initialised;
} as5048b_t;

/** @brief Bind context to caller-opened I²C bus. */
alp_status_t as5048b_init(as5048b_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read 14-bit absolute angle in raw counts (0..16383).
 *
 * @param dev          Initialised context.
 * @param angle_out    Output: angle in counts.
 * @return `ALP_OK` on success.
 */
alp_status_t as5048b_read_angle(as5048b_t *dev, uint16_t *angle_out);

/** @brief Release driver context. */
void as5048b_deinit(as5048b_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_AS5048A_B_H */
