/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file imx477.h
 * @brief Sony IMX477 12.3 MP MIPI CSI-2 sensor (Raspberry Pi HQ Camera).
 *
 * 1/2.3-inch 4056 × 3040 Bayer sensor used in the RPi HQ Camera
 * module.  SCCB config side only; pixel data flows over MIPI CSI-2.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Driver status: [stub-impl]
 */

#ifndef ALP_CHIPS_IMX477_H
#define ALP_CHIPS_IMX477_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IMX477_I2C_ADDR 0x1Au

#define IMX477_REG_MODEL_ID_HI 0x0016u
#define IMX477_REG_MODEL_ID_LO 0x0017u
#define IMX477_CHIP_ID         0x0477u

typedef struct {
    alp_i2c_t *bus;
    uint8_t    addr;
    bool       initialised;
} imx477_t;

/** @brief Bind context and verify chip ID. */
alp_status_t imx477_init(imx477_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** @brief Read MODEL_ID. */
alp_status_t imx477_read_id(imx477_t *dev, uint16_t *id_out);

/** @brief Issue a software reset (0x0103 bit 0). */
alp_status_t imx477_soft_reset(imx477_t *dev);

/** @brief Release driver context.  NULL tolerated. */
void imx477_deinit(imx477_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_IMX477_H */
