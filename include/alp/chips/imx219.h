/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file imx219.h
 * @brief Sony IMX219 8 MP MIPI CSI-2 sensor (Raspberry Pi Camera v2).
 *
 * Bayer-Quad-Pixel 3280 × 2464 sensor.  SCCB config side only;
 * pixel data flows over MIPI CSI-2.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Driver status: [stub-impl]
 *
 * Datasheet: Sony IMX219 (no public datasheet; reverse-engineered
 * register map per the Raspberry Pi kernel driver + Linaro NDA
 * extract).  Maintainer-internal datasheet copy required for the
 * vendor init script.
 */

#ifndef ALP_CHIPS_IMX219_H
#define ALP_CHIPS_IMX219_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMX219_I2C_ADDR 0x10u

#define IMX219_REG_MODEL_ID_HI 0x0000u
#define IMX219_REG_MODEL_ID_LO 0x0001u
#define IMX219_CHIP_ID         0x0219u

typedef struct {
    alp_i2c_t *bus;
    uint8_t    addr;
    bool       initialised;
} imx219_t;

/** @brief Bind context and verify chip ID. */
alp_status_t imx219_init(imx219_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** @brief Read MODEL_ID for liveness checks. */
alp_status_t imx219_read_id(imx219_t *dev, uint16_t *id_out);

/** @brief Issue a software reset (0x0103 bit 0). */
alp_status_t imx219_soft_reset(imx219_t *dev);

/** @brief Release driver context.  NULL tolerated. */
void imx219_deinit(imx219_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_IMX219_H */
