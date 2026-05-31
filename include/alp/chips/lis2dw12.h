/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lis2dw12.h
 * @brief STMicroelectronics LIS2DW12 3-axis ultra-low-power accelerometer.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Public surface consumed by alp-studio block `blk_accel_lis2dw12`.
 * Symbols carry the chip's natural prefix `lis2dw12_*` — no `alp_`.
 *
 * v0.2 scope: I2C only, ODR + full-scale + power-mode configuration,
 * raw 14-bit accel reads.  SPI lands in v0.3 alongside FIFO support.
 *
 * Datasheet: STMicroelectronics LIS2DW12 (DocID030334, rev 4, Mar 2018).
 */

#ifndef ALP_CHIPS_LIS2DW12_H
#define ALP_CHIPS_LIS2DW12_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default 7-bit I2C addresses (SDO/SA0 pin selects). */
#define LIS2DW12_I2C_ADDR_LOW   0x18
#define LIS2DW12_I2C_ADDR_HIGH  0x19

/** WHO_AM_I register value the chip returns. */
#define LIS2DW12_WHO_AM_I_VAL   0x44

/** Output data rate (CTRL1 bits[7:4]).  Actual frequency depends on power mode. */
typedef enum {
    LIS2DW12_ODR_OFF      = 0x0,
    LIS2DW12_ODR_1_6_HZ   = 0x1,    /**< Low-power mode only; 12.5 Hz in HP. */
    LIS2DW12_ODR_12_5_HZ  = 0x2,
    LIS2DW12_ODR_25_HZ    = 0x3,
    LIS2DW12_ODR_50_HZ    = 0x4,
    LIS2DW12_ODR_100_HZ   = 0x5,
    LIS2DW12_ODR_200_HZ   = 0x6,
    LIS2DW12_ODR_400_HZ   = 0x7,    /**< High-performance mode only. */
    LIS2DW12_ODR_800_HZ   = 0x8,    /**< High-performance mode only. */
    LIS2DW12_ODR_1600_HZ  = 0x9     /**< High-performance mode only. */
} lis2dw12_odr_t;

/** Full-scale range (CTRL6 bits[5:4]). */
typedef enum {
    LIS2DW12_FS_2G  = 0x0,
    LIS2DW12_FS_4G  = 0x1,
    LIS2DW12_FS_8G  = 0x2,
    LIS2DW12_FS_16G = 0x3
} lis2dw12_fs_t;

/** Power / resolution mode (CTRL1 bits[3:2] paired with LP_MODE bits[1:0]). */
typedef enum {
    LIS2DW12_MODE_LOW_POWER_12BIT = 0x0,    /**< 12-bit, lowest current. */
    LIS2DW12_MODE_LOW_POWER_14BIT = 0x1,
    LIS2DW12_MODE_HIGH_PERF_14BIT = 0x2,    /**< Full bandwidth. */
    LIS2DW12_MODE_SINGLE_SHOT     = 0x3     /**< One sample on demand. */
} lis2dw12_mode_t;

/** Three-axis sample, raw 16-bit signed counts (left-justified). */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} lis2dw12_axes_t;

/** Driver context.  Treat as opaque. */
typedef struct {
    alp_i2c_t       *bus;
    uint8_t          addr;
    lis2dw12_fs_t    fs;
    lis2dw12_mode_t  mode;
    bool             initialised;
} lis2dw12_t;

/**
 * @brief Bind a driver context to an open I2C bus.
 *
 * Reads WHO_AM_I and verifies it matches @ref LIS2DW12_WHO_AM_I_VAL.
 * Does not start sampling — caller selects ODR + full-scale + mode
 * via @ref lis2dw12_set_accel.
 *
 * @return ALP_OK on success; ALP_ERR_IO on WHO_AM_I mismatch.
 */
alp_status_t lis2dw12_init(lis2dw12_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** Read WHO_AM_I for liveness checks. */
alp_status_t lis2dw12_read_id(lis2dw12_t *dev, uint8_t *id_out);

/** Configure ODR + full-scale + power mode in a single call. */
alp_status_t lis2dw12_set_accel(lis2dw12_t *dev,
                                lis2dw12_odr_t odr,
                                lis2dw12_fs_t fs,
                                lis2dw12_mode_t mode);

/** Read the current accelerometer sample (raw int16 counts). */
alp_status_t lis2dw12_read_accel(lis2dw12_t *dev, lis2dw12_axes_t *out);

/** Read the on-die temperature sensor (raw int16; 8-bit resolution). */
alp_status_t lis2dw12_read_temp(lis2dw12_t *dev, int16_t *temp_raw);

/** Release the driver context.  Does not power down the chip. */
void         lis2dw12_deinit(lis2dw12_t *dev);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_CHIPS_LIS2DW12_H */
