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
 *   - Each register holds a 16-bit value, addressed by a single
 *     8-bit register index (NOT 16-bit addressing).
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

/** Which axis/axes had fresh data available (STATUS.drdy_acc /
 *  STATUS.drdy_gyr, BST-BMI323-DS000-13 Rev 1.7, p.66). */
typedef struct {
	bool accel;
	bool gyro;
} bmi323_data_ready_t;

/**
 * Physical interrupt pin.  On the E1M EVK (UG-E1M-001) INT1 is a direct
 * SoC/E1M edge pin (E2 pin F3, pad IO15) -- this driver only exposes the
 * registers, the caller wires the GPIO.
 */
typedef enum { BMI323_INT_PIN1 = 0, BMI323_INT_PIN2 = 1 } bmi323_int_pin_t;

/** INT1/INT2 active level (IO_INT_CTRL.int1_lvl / int2_lvl,
 *  BST-BMI323-DS000-13 Rev 1.7, p.102). */
typedef enum {
	BMI323_INT_LEVEL_ACTIVE_LOW  = 0,
	BMI323_INT_LEVEL_ACTIVE_HIGH = 1
} bmi323_int_level_t;

/** INT1/INT2 drive circuit (IO_INT_CTRL.int1_od / int2_od,
 *  BST-BMI323-DS000-13 Rev 1.7, p.102). */
typedef enum { BMI323_INT_DRIVE_PUSH_PULL = 0, BMI323_INT_DRIVE_OPEN_DRAIN = 1 } bmi323_int_drive_t;

/** Electrical configuration for one INT pin. */
typedef struct {
	bmi323_int_level_t level;
	bmi323_int_drive_t drive;
	bool               output_enable; /**< IO_INT_CTRL.int1/2_output_en. */
} bmi323_int_pin_config_t;

/** Interrupt clear behaviour, device-wide -- not per pin (INT_CONF.int_latch,
 *  BST-BMI323-DS000-13 Rev 1.7, p.103). */
typedef enum {
	BMI323_INT_LATCH_NON_LATCHED = 0, /**< Auto-clears; see p.103 for the exact timing. */
	BMI323_INT_LATCH_PERMANENT   = 1  /**< Stays asserted until the INT_STATUS_* flag is read. */
} bmi323_int_latch_t;

/**
 * Where a data-ready source is routed (INT_MAP2 2-bit fields,
 * BST-BMI323-DS000-13 Rev 1.7, pp.106-107).  Same 4 values on every
 * INT_MAP2 field this driver touches.
 */
typedef enum {
	BMI323_INT_ROUTE_DISABLED = 0x0,
	BMI323_INT_ROUTE_INT1     = 0x1,
	BMI323_INT_ROUTE_INT2     = 0x2,
	BMI323_INT_ROUTE_I3C_IBI  = 0x3
} bmi323_int_route_t;

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
 * Each BMI323 register holds a 16-bit value at a single 8-bit
 * register index (not 16-bit addressing).  Reads return a 2-byte
 * dummy prefix that the wrapper strips internally.
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

/**
 * @brief Configure one INT pin's electrical behaviour (level/drive/output-enable).
 *
 * Read-modify-write: IO_INT_CTRL (0x38) packs both INT1 (bits[2:0]) and
 * INT2 (bits[10:8]) into one 16-bit register, so this only touches the
 * bits belonging to `pin`.
 *
 * This does not by itself route anything to `pin` -- pair with
 * @ref bmi323_set_int_latch and @ref bmi323_route_data_ready_int.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY (uninitialised) / ALP_ERR_INVAL
 *   (`pin` not a declared enum member, or `cfg` NULL).
 */
alp_status_t
bmi323_configure_int_pin(bmi323_t *dev, bmi323_int_pin_t pin, const bmi323_int_pin_config_t *cfg);

/**
 * @brief Set the device-wide interrupt latch mode (INT_CONF.int_latch).
 *
 * Full write: INT_CONF (0x39) has no other live field (reserved bits
 * write 0).
 *
 * @return ALP_OK / ALP_ERR_NOT_READY (uninitialised).
 */
alp_status_t bmi323_set_int_latch(bmi323_t *dev, bmi323_int_latch_t mode);

/**
 * @brief Route the accelerometer and/or gyroscope data-ready interrupt.
 *
 * Read-modify-write: INT_MAP2 (0x3B) also carries temp/FIFO/tap/i3c/err
 * routing this driver does not manage, so only the acc_drdy_int
 * (bits[11:10]) and gyr_drdy_int (bits[9:8]) fields are touched.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY (uninitialised).
 */
alp_status_t bmi323_route_data_ready_int(bmi323_t          *dev,
                                         bmi323_int_route_t accel_route,
                                         bmi323_int_route_t gyro_route);

/**
 * @brief Poll STATUS.drdy_acc / STATUS.drdy_gyr (BST-BMI323-DS000-13
 *   Rev 1.7, p.66) without wiring a physical interrupt pin.
 *
 * Both flags are clear-on-read; reading this also clears
 * STATUS.drdy_acc the same way reading ACC_DATA_X..Z does (Rev 1.7,
 * p.23 "Accelerometer Data Ready Notification").  This complements, it
 * does not replace, the fixed accel/gyro start-up waits in
 * @ref bmi323_set_accel / @ref bmi323_set_gyro -- polling before the
 * relevant tA,SU/tG,SU floor has elapsed is meaningless, the flag
 * simply has not been asserted yet.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY (uninitialised) / ALP_ERR_INVAL
 *   (`out` NULL).
 */
alp_status_t bmi323_data_ready(bmi323_t *dev, bmi323_data_ready_t *out);

/** Release the driver context. */
void bmi323_deinit(bmi323_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_BMI323_H */
