/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file lsm6dso.h
 * @brief STMicroelectronics LSM6DSO 6-axis IMU driver.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Public surface consumed by alp-studio block `blk_imu_lsm6dso`.
 * Symbols carry the chip's natural prefix `lsm6dso_*` — no `alp_`
 * (chip drivers are bindings to third-party silicon).
 *
 * I2C-only in v0.1.  SPI lands in v0.2 along with `lsm6dso_init_spi`.
 *
 * Datasheet: STMicroelectronics LSM6DSO (DocID032086).
 */

#ifndef ALP_CHIPS_LSM6DSO_H
#define ALP_CHIPS_LSM6DSO_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default 7-bit I2C addresses (SDO/SA0 pin selects). */
#define LSM6DSO_I2C_ADDR_LOW   0x6A   /**< SDO/SA0 tied low. */
#define LSM6DSO_I2C_ADDR_HIGH  0x6B   /**< SDO/SA0 tied high. */

/** WHO_AM_I register value the chip returns for self-identification. */
#define LSM6DSO_WHO_AM_I_VAL   0x6C

/** Accelerometer / gyroscope output data rates. */
typedef enum {
    LSM6DSO_ODR_OFF      = 0x0,
    LSM6DSO_ODR_12_5_HZ  = 0x1,
    LSM6DSO_ODR_26_HZ    = 0x2,
    LSM6DSO_ODR_52_HZ    = 0x3,
    LSM6DSO_ODR_104_HZ   = 0x4,
    LSM6DSO_ODR_208_HZ   = 0x5,
    LSM6DSO_ODR_416_HZ   = 0x6,
    LSM6DSO_ODR_833_HZ   = 0x7,
    LSM6DSO_ODR_1660_HZ  = 0x8,
    LSM6DSO_ODR_3330_HZ  = 0x9,
    LSM6DSO_ODR_6660_HZ  = 0xA
} lsm6dso_odr_t;

/** Accelerometer full-scale range. */
typedef enum {
    LSM6DSO_ACCEL_FS_2G  = 0x0,
    LSM6DSO_ACCEL_FS_16G = 0x1,
    LSM6DSO_ACCEL_FS_4G  = 0x2,
    LSM6DSO_ACCEL_FS_8G  = 0x3
} lsm6dso_accel_fs_t;

/** Gyroscope full-scale range (degrees per second). */
typedef enum {
    LSM6DSO_GYRO_FS_250_DPS  = 0x0,
    LSM6DSO_GYRO_FS_500_DPS  = 0x1,
    LSM6DSO_GYRO_FS_1000_DPS = 0x2,
    LSM6DSO_GYRO_FS_2000_DPS = 0x3
} lsm6dso_gyro_fs_t;

/** Three-axis sample, raw 16-bit signed counts (LSB units). */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} lsm6dso_axes_t;

/** Driver context.  Treat as opaque. */
typedef struct {
    alp_i2c_t          *bus;
    uint8_t             addr;       /**< 7-bit I2C address. */
    lsm6dso_accel_fs_t  accel_fs;
    lsm6dso_gyro_fs_t   gyro_fs;
    bool                initialised;
} lsm6dso_t;

/**
 * @brief Bind a driver context to an open I2C bus.
 *
 * Reads WHO_AM_I and verifies it matches @ref LSM6DSO_WHO_AM_I_VAL.
 * Does not touch ODR or full-scale registers — the caller selects
 * those next via @ref lsm6dso_set_accel and @ref lsm6dso_set_gyro.
 *
 * @return ALP_OK on success, or an error from the underlying I2C
 *         transfer.  Returns ALP_ERR_IO when the WHO_AM_I check
 *         fails (chip not present or not LSM6DSO).
 */
alp_status_t lsm6dso_init(lsm6dso_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** Read the WHO_AM_I register. Useful for liveness checks post-init. */
alp_status_t lsm6dso_read_id(lsm6dso_t *dev, uint8_t *id_out);

/** Configure accelerometer ODR + full-scale range. */
alp_status_t lsm6dso_set_accel(lsm6dso_t *dev,
                               lsm6dso_odr_t odr,
                               lsm6dso_accel_fs_t fs);

/** Configure gyroscope ODR + full-scale range. */
alp_status_t lsm6dso_set_gyro(lsm6dso_t *dev,
                              lsm6dso_odr_t odr,
                              lsm6dso_gyro_fs_t fs);

/** Read the current accelerometer sample (raw int16 counts). */
alp_status_t lsm6dso_read_accel(lsm6dso_t *dev, lsm6dso_axes_t *out);

/** Read the current gyroscope sample (raw int16 counts). */
alp_status_t lsm6dso_read_gyro(lsm6dso_t *dev, lsm6dso_axes_t *out);

/** Read the on-die temperature sensor (raw int16; LSB ≈ 1/256 °C). */
alp_status_t lsm6dso_read_temp(lsm6dso_t *dev, int16_t *temp_raw);

/** Release the driver context.  Does not power down the chip. */
void         lsm6dso_deinit(lsm6dso_t *dev);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_CHIPS_LSM6DSO_H */
