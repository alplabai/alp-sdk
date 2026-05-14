/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file maxim_max9295_9296.h
 * @brief ADI (Maxim) MAX9295A + MAX9296A GMSL2 camera SerDes.
 *
 * 6 Gbps Gigabit Multimedia Serial Link 2 over coax.  Same overall
 * shape as TI's FPD-Link III pair (`ti_ds90ub953_954`) but at
 * 6 Gbps with native automotive-grade qualification.  Dominant
 * SerDes in ADAS production today.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl] — chip-ID + soft reset only.
 *
 * Datasheets: MAX9295A (Rev 5), MAX9296A (Rev 5).
 */

#ifndef ALP_CHIPS_MAXIM_MAX9295_9296_H
#define ALP_CHIPS_MAXIM_MAX9295_9296_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MAX9295_I2C_ADDR_DEFAULT 0x40u /**< Serialiser. */
#define MAX9296_I2C_ADDR_DEFAULT 0x48u /**< Deserialiser. */

#define MAX929x_REG_DEV_ID  0x000Du
#define MAX929x_REG_DEV_REV 0x000Eu

#define MAX9295_DEV_ID 0x91u
#define MAX9296_DEV_ID 0x94u

typedef struct {
    alp_i2c_t *bus;
    uint8_t    ser_addr;
    uint8_t    des_addr;
    bool       initialised;
} maxim_gmsl2_t;

/**
 * @brief Probe both ends of the GMSL2 pair over I²C.
 *
 * Verifies MAX9295A serialiser DEV_ID == 0x91 and MAX9296A
 * deserialiser DEV_ID == 0x94.
 */
alp_status_t maxim_gmsl2_init(maxim_gmsl2_t *dev,
                              alp_i2c_t     *bus,
                              uint8_t        ser_addr,
                              uint8_t        des_addr);

/** @brief Read DEV_ID from the deserialiser. */
alp_status_t maxim_gmsl2_read_des_id(maxim_gmsl2_t *dev, uint8_t *id_out);

/** @brief Read DEV_ID from the serialiser. */
alp_status_t maxim_gmsl2_read_ser_id(maxim_gmsl2_t *dev, uint8_t *id_out);

/** @brief Issue soft reset on both ends (CTRL0 bit 6). */
alp_status_t maxim_gmsl2_soft_reset(maxim_gmsl2_t *dev);

/** @brief Release driver context.  NULL tolerated. */
void maxim_gmsl2_deinit(maxim_gmsl2_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_MAXIM_MAX9295_9296_H */
