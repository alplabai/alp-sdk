/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file icm42670.h
 * @brief TDK InvenSense ICM-42670-P 6-axis IMU driver.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Public surface consumed by alp-studio block `blk_imu_icm42670`.
 * Symbols carry the chip's natural prefix `icm42670_*` — no `alp_`
 * (chip drivers are bindings to third-party silicon).
 *
 * The ICM-42670-P is one of the on-board IMUs on the E1M EVK
 * (UG-E1M-001).  Compared to the LSM6DSO it adds APEX (algorithm
 * processing engine) features (pedometer, tilt detection, freefall)
 * driven by an internal DMP, but this v0.2 driver covers only the
 * raw accel/gyro/temperature path; APEX integration arrives in v0.3.
 *
 * I²C-only in v0.2.  SPI lands alongside the v0.3 DMP support.
 *
 * Datasheet: TDK InvenSense ICM-42670-P (DS-000451 v1.6, Apr 2024).
 */

#ifndef ALP_CHIPS_ICM42670_H
#define ALP_CHIPS_ICM42670_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default 7-bit I2C addresses (AP_AD0 strap selects). */
#define ICM42670_I2C_ADDR_LOW   0x68    /**< AP_AD0 tied low. */
#define ICM42670_I2C_ADDR_HIGH  0x69    /**< AP_AD0 tied high. */

/** WHO_AM_I (register 0x75) value the chip returns. */
#define ICM42670_WHO_AM_I_VAL   0x67

/** Accelerometer / gyroscope output data rates (PWR_MGMT0 / ACCEL_CONFIG0 fields). */
typedef enum {
    ICM42670_ODR_OFF      = 0x0,
    ICM42670_ODR_1_5625_HZ = 0xF,   /**< Low-power only. */
    ICM42670_ODR_3_125_HZ = 0xE,    /**< Low-power only. */
    ICM42670_ODR_6_25_HZ  = 0xD,    /**< Low-power only. */
    ICM42670_ODR_12_5_HZ  = 0xC,
    ICM42670_ODR_25_HZ    = 0xB,
    ICM42670_ODR_50_HZ    = 0xA,
    ICM42670_ODR_100_HZ   = 0x8,
    ICM42670_ODR_200_HZ   = 0x7,
    ICM42670_ODR_400_HZ   = 0x6,
    ICM42670_ODR_800_HZ   = 0x5,
    ICM42670_ODR_1600_HZ  = 0x4
} icm42670_odr_t;

/** Accelerometer full-scale range (ACCEL_CONFIG0 bits[6:5]). */
typedef enum {
    ICM42670_ACCEL_FS_16G = 0x0,
    ICM42670_ACCEL_FS_8G  = 0x1,
    ICM42670_ACCEL_FS_4G  = 0x2,
    ICM42670_ACCEL_FS_2G  = 0x3
} icm42670_accel_fs_t;

/** Gyroscope full-scale range (GYRO_CONFIG0 bits[6:5]). */
typedef enum {
    ICM42670_GYRO_FS_2000_DPS = 0x0,
    ICM42670_GYRO_FS_1000_DPS = 0x1,
    ICM42670_GYRO_FS_500_DPS  = 0x2,
    ICM42670_GYRO_FS_250_DPS  = 0x3
} icm42670_gyro_fs_t;

/** Three-axis sample, raw 16-bit signed counts. */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} icm42670_axes_t;

/** Driver context.  Treat as opaque. */
typedef struct {
    alp_i2c_t            *bus;
    uint8_t               addr;
    icm42670_accel_fs_t   accel_fs;
    icm42670_gyro_fs_t    gyro_fs;
    bool                  initialised;
} icm42670_t;

/**
 * @brief Bind a driver context to an open I²C bus and verify chip ID.
 *
 * Reads WHO_AM_I and verifies it matches @ref ICM42670_WHO_AM_I_VAL.
 * Does not start sampling -- caller selects ODR + FS via
 * @ref icm42670_set_accel and @ref icm42670_set_gyro.
 *
 * @return ALP_OK on success; ALP_ERR_IO on WHO_AM_I mismatch.
 */
alp_status_t icm42670_init(icm42670_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** Read WHO_AM_I for liveness checks. */
alp_status_t icm42670_read_id(icm42670_t *dev, uint8_t *id_out);

/** Configure accelerometer ODR + full-scale range. */
alp_status_t icm42670_set_accel(icm42670_t *dev,
                                icm42670_odr_t odr,
                                icm42670_accel_fs_t fs);

/** Configure gyroscope ODR + full-scale range. */
alp_status_t icm42670_set_gyro(icm42670_t *dev,
                               icm42670_odr_t odr,
                               icm42670_gyro_fs_t fs);

/** Read the current accelerometer sample (raw int16 counts). */
alp_status_t icm42670_read_accel(icm42670_t *dev, icm42670_axes_t *out);

/** Read the current gyroscope sample (raw int16 counts). */
alp_status_t icm42670_read_gyro(icm42670_t *dev, icm42670_axes_t *out);

/** Read the on-die temperature sensor (raw int16; LSB ≈ 1/132.48 °C). */
alp_status_t icm42670_read_temp(icm42670_t *dev, int16_t *temp_raw);

/** Release the driver context.  Does not power down the chip. */
void          icm42670_deinit(icm42670_t *dev);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_CHIPS_ICM42670_H */
