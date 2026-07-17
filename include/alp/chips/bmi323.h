/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file bmi323.h
 * @brief Bosch BMI323 6-axis IMU driver.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Public surface consumed by alp-studio block `blk_imu_bmi323`.
 * Symbols carry the chip's natural prefix `bmi323_*` — no `alp_`.
 *
 * The BMI323 is the second on-board IMU on the E1M EVK alongside
 * the ICM-42670-P; apps that want sensor fusion or redundancy can
 * read both.  Notable quirks:
 *
 *   - 16-bit register addressing (most IMUs use 8-bit).
 *   - Each register read returns a 2-byte dummy prefix that
 *     callers must skip (Bosch quirk for SPI alignment, also
 *     applies on I²C for consistency).
 *
 * v0.2 scope: I²C only, raw accel/gyro/temp reads, ODR + FS config.
 * v0.3 adds the on-chip Bosch fusion engine + virtual sensors.
 *
 * Datasheet: Bosch BMI323 (BST-BMI323-DS000 v1.5, Mar 2024).
 */

#ifndef ALP_CHIPS_BMI323_H
#define ALP_CHIPS_BMI323_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default 7-bit I2C addresses (SDO strap selects). */
#define BMI323_I2C_ADDR_LOW  0x68
#define BMI323_I2C_ADDR_HIGH 0x69

/** CHIP_ID (register 0x00) value. */
#define BMI323_CHIP_ID 0x43

/** Output data rate (ACC_CONF / GYR_CONF bits[3:0]). */
typedef enum {
	BMI323_ODR_0_78125_HZ = 0x1,
	BMI323_ODR_1_5625_HZ  = 0x2,
	BMI323_ODR_3_125_HZ   = 0x3,
	BMI323_ODR_6_25_HZ    = 0x4,
	BMI323_ODR_12_5_HZ    = 0x5,
	BMI323_ODR_25_HZ      = 0x6,
	BMI323_ODR_50_HZ      = 0x7,
	BMI323_ODR_100_HZ     = 0x8,
	BMI323_ODR_200_HZ     = 0x9,
	BMI323_ODR_400_HZ     = 0xA,
	BMI323_ODR_800_HZ     = 0xB,
	BMI323_ODR_1600_HZ    = 0xC,
	BMI323_ODR_3200_HZ    = 0xD,
	BMI323_ODR_6400_HZ    = 0xE
} bmi323_odr_t;

/** Accelerometer full-scale range (ACC_CONF bits[6:4]). */
typedef enum {
	BMI323_ACCEL_FS_2G  = 0x0,
	BMI323_ACCEL_FS_4G  = 0x1,
	BMI323_ACCEL_FS_8G  = 0x2,
	BMI323_ACCEL_FS_16G = 0x3
} bmi323_accel_fs_t;

/** Gyroscope full-scale range (GYR_CONF bits[6:4]). */
typedef enum {
	BMI323_GYRO_FS_125_DPS  = 0x0,
	BMI323_GYRO_FS_250_DPS  = 0x1,
	BMI323_GYRO_FS_500_DPS  = 0x2,
	BMI323_GYRO_FS_1000_DPS = 0x3,
	BMI323_GYRO_FS_2000_DPS = 0x4
} bmi323_gyro_fs_t;

/** Three-axis sample, raw 16-bit signed counts. */
typedef struct {
	int16_t x;
	int16_t y;
	int16_t z;
} bmi323_axes_t;

/** Driver context.  Treat as opaque. */
typedef struct {
	alp_i2c_t        *bus;
	uint8_t           addr;
	bmi323_accel_fs_t accel_fs;
	bmi323_gyro_fs_t  gyro_fs;
	bool              initialised;
} bmi323_t;

/**
 * @brief Bind a driver context to an open I²C bus and verify chip ID.
 *
 * The BMI323 uses 16-bit register addressing -- the wrapper writes
 * a 2-byte address pointer for every transaction.  Reads return
 * a 2-byte dummy prefix that the wrapper strips internally.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_IO (wrong CHIP_ID).
 */
alp_status_t bmi323_init(bmi323_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** Read CHIP_ID for liveness checks. */
alp_status_t bmi323_read_id(bmi323_t *dev, uint8_t *id_out);

/**
 * @brief Configure accelerometer ODR + full-scale range.
 * @return ALP_OK / ALP_ERR_NOT_READY (uninitialised) / ALP_ERR_INVAL
 *   (`odr` or `fs` is not a declared enum member).
 */
alp_status_t bmi323_set_accel(bmi323_t *dev, bmi323_odr_t odr, bmi323_accel_fs_t fs);

/**
 * @brief Configure gyroscope ODR + full-scale range.
 * @return ALP_OK / ALP_ERR_NOT_READY (uninitialised) / ALP_ERR_INVAL
 *   (`odr` or `fs` is not a declared enum member).
 */
alp_status_t bmi323_set_gyro(bmi323_t *dev, bmi323_odr_t odr, bmi323_gyro_fs_t fs);

/** Read the current accelerometer sample (raw int16 counts). */
alp_status_t bmi323_read_accel(bmi323_t *dev, bmi323_axes_t *out);

/** Read the current gyroscope sample (raw int16 counts). */
alp_status_t bmi323_read_gyro(bmi323_t *dev, bmi323_axes_t *out);

/** Read the on-die temperature sensor (raw int16; offset = 0 °C @ 23.0). */
alp_status_t bmi323_read_temp(bmi323_t *dev, int16_t *temp_raw);

/** Release the driver context. */
void bmi323_deinit(bmi323_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_BMI323_H */
