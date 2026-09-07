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
 * Datasheet: TDK InvenSense ICM-42670-P, DS-000451 Rev 1.0 (04/15/2021).
 *   Every page citation in this header and in icm42670.c is verified
 *   against this revision; do not relabel a cited page number to a
 *   different revision without re-verifying it there.
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
#define ICM42670_I2C_ADDR_LOW  0x68 /**< AP_AD0 tied low. */
#define ICM42670_I2C_ADDR_HIGH 0x69 /**< AP_AD0 tied high. */

/** WHO_AM_I (register 0x75) value the chip returns. */
#define ICM42670_WHO_AM_I_VAL 0x67

/**
 * Accelerometer / gyroscope output data rates (ACCEL_CONFIG0 /
 * GYRO_CONFIG0 fields -- PWR_MGMT0 carries no ODR field, it only
 * selects the power mode).
 *
 * @note ::ICM42670_ODR_OFF names the all-zero code, which is a
 *   *reserved* encoding on both ACCEL_ODR and GYRO_ODR (TDK
 *   DS-000451 Rev 1.0, p.57 / p.56) -- it is not a valid "disable
 *   sampling via ODR" value.  Both @ref icm42670_set_accel and
 *   @ref icm42670_set_gyro reject it with ::ALP_ERR_INVAL; use
 *   PWR_MGMT0 (not yet exposed) to actually power an engine down.
 */
typedef enum {
	ICM42670_ODR_OFF       = 0x0, /**< Reserved encoding -- always rejected, see @note above. */
	ICM42670_ODR_1_5625_HZ = 0xF, /**< Accel low-power only; reserved on GYRO_ODR. */
	ICM42670_ODR_3_125_HZ  = 0xE, /**< Accel low-power only; reserved on GYRO_ODR. */
	ICM42670_ODR_6_25_HZ   = 0xD, /**< Accel low-power only; reserved on GYRO_ODR. */
	ICM42670_ODR_12_5_HZ   = 0xC,
	ICM42670_ODR_25_HZ     = 0xB,
	ICM42670_ODR_50_HZ     = 0xA,
	ICM42670_ODR_100_HZ    = 0x9,
	ICM42670_ODR_200_HZ    = 0x8,
	ICM42670_ODR_400_HZ    = 0x7,
	ICM42670_ODR_800_HZ    = 0x6,
	ICM42670_ODR_1600_HZ   = 0x5 /**< 1.6 kHz, low-noise. */
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

/**
 * Physical interrupt pin.  The E1M EVK (UG-E1M-001) routes both INT1 and
 * INT2 to expander inputs (TCAL9538 U35 P4 / P5) -- this driver only
 * exposes the registers, the caller wires the GPIO.
 */
typedef enum { ICM42670_INT_PIN1 = 0, ICM42670_INT_PIN2 = 1 } icm42670_int_pin_t;

/** INT1/INT2 pulse-vs-latch behaviour (INT_CONFIG bit 2 / bit 5, TDK
 *  DS-000451 Rev 1.0, p.50). */
typedef enum {
	ICM42670_INT_MODE_PULSED  = 0, /**< Pulsed mode. */
	ICM42670_INT_MODE_LATCHED = 1  /**< Latched mode. */
} icm42670_int_mode_t;

/** INT1/INT2 drive circuit (INT_CONFIG bit 1 / bit 4, TDK DS-000451
 *  Rev 1.0, p.50). */
typedef enum {
	ICM42670_INT_DRIVE_OPEN_DRAIN = 0,
	ICM42670_INT_DRIVE_PUSH_PULL  = 1
} icm42670_int_drive_t;

/** INT1/INT2 polarity (INT_CONFIG bit 0 / bit 3, TDK DS-000451 Rev 1.0,
 *  p.50). */
typedef enum {
	ICM42670_INT_POLARITY_ACTIVE_LOW  = 0,
	ICM42670_INT_POLARITY_ACTIVE_HIGH = 1
} icm42670_int_polarity_t;

/** Electrical configuration for one INT pin (mode + drive + polarity). */
typedef struct {
	icm42670_int_mode_t     mode;
	icm42670_int_drive_t    drive;
	icm42670_int_polarity_t polarity;
} icm42670_int_pin_config_t;

/**
 * INT_SOURCE0 / INT_SOURCE3 routing bits (TDK DS-000451 Rev 1.0, p.63-64
 * -- INT_SOURCE0 routes to INT1, INT_SOURCE3 to INT2; both registers
 * share this bit layout).  OR members together to route more than one
 * source to the same pin.  INT_SOURCE1/INT_SOURCE2/INT_SOURCE4/
 * INT_SOURCE5 (WOM axes, SMD, I3C protocol error) are out of scope for
 * this driver.
 */
typedef enum {
	ICM42670_INT_SRC_AGC_RDY    = 1u << 0,
	ICM42670_INT_SRC_FIFO_FULL  = 1u << 1,
	ICM42670_INT_SRC_FIFO_THS   = 1u << 2,
	ICM42670_INT_SRC_DATA_RDY   = 1u << 3,
	ICM42670_INT_SRC_RESET_DONE = 1u << 4,
	ICM42670_INT_SRC_PLL_RDY    = 1u << 5,
	ICM42670_INT_SRC_FSYNC      = 1u << 6,
	ICM42670_INT_SRC_ST_DONE    = 1u << 7
} icm42670_int_src_t;

/** Driver context.  Treat as opaque. */
typedef struct {
	alp_i2c_t          *bus;
	uint8_t             addr;
	icm42670_accel_fs_t accel_fs;
	icm42670_gyro_fs_t  gyro_fs;
	bool                initialised;
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

/**
 * @brief Configure accelerometer ODR + full-scale range.
 * @return ALP_OK / ALP_ERR_NOT_READY (uninitialised) / ALP_ERR_INVAL
 *   (`odr` is not a code ACCEL_ODR accepts -- rejects ::ICM42670_ODR_OFF
 *   and any code reserved on ACCEL_CONFIG0, p.57).
 */
alp_status_t icm42670_set_accel(icm42670_t *dev, icm42670_odr_t odr, icm42670_accel_fs_t fs);

/**
 * @brief Configure gyroscope ODR + full-scale range.
 * @return ALP_OK / ALP_ERR_NOT_READY (uninitialised) / ALP_ERR_INVAL
 *   (`odr` is not a code GYRO_ODR accepts -- rejects ::ICM42670_ODR_OFF
 *   and any code reserved on GYRO_CONFIG0, p.56, which is a narrower
 *   set than ACCEL_CONFIG0's: no 1.5625/3.125/6.25 Hz on gyro).
 */
alp_status_t icm42670_set_gyro(icm42670_t *dev, icm42670_odr_t odr, icm42670_gyro_fs_t fs);

/** Read the current accelerometer sample (raw int16 counts). */
alp_status_t icm42670_read_accel(icm42670_t *dev, icm42670_axes_t *out);

/** Read the current gyroscope sample (raw int16 counts). */
alp_status_t icm42670_read_gyro(icm42670_t *dev, icm42670_axes_t *out);

/** Read the on-die temperature sensor (raw int16; LSB ≈ 1/132.48 °C). */
alp_status_t icm42670_read_temp(icm42670_t *dev, int16_t *temp_raw);

/**
 * @brief Configure one INT pin's electrical behaviour (mode/drive/polarity).
 *
 * Read-modify-write: INT_CONFIG (0x06) packs both INT1 and INT2's fields
 * into one register, so this only touches the three bits belonging to
 * `pin` and leaves the other pin's configuration untouched.
 *
 * This does not by itself route anything to `pin` -- pair with
 * @ref icm42670_route_int.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY (uninitialised) / ALP_ERR_INVAL
 *   (`pin` not a declared enum member, or `cfg` NULL).
 */
alp_status_t icm42670_configure_int_pin(icm42670_t                      *dev,
                                        icm42670_int_pin_t               pin,
                                        const icm42670_int_pin_config_t *cfg);

/**
 * @brief Route a set of interrupt sources to `pin`.
 *
 * Full write to INT_SOURCE0 (`pin` == ::ICM42670_INT_PIN1) or
 * INT_SOURCE3 (::ICM42670_INT_PIN2), same convention as
 * @ref icm42670_set_accel / @ref icm42670_set_gyro -- it replaces
 * whatever was previously routed to that pin rather than merging with
 * it, so pass the OR of every ::icm42670_int_src_t the caller wants
 * live on `pin`.
 *
 * @param source_mask OR of ::icm42670_int_src_t members; 0 disables
 *   routing to `pin` entirely.
 * @return ALP_OK / ALP_ERR_NOT_READY (uninitialised) / ALP_ERR_INVAL
 *   (`pin` not a declared enum member).
 */
alp_status_t icm42670_route_int(icm42670_t *dev, icm42670_int_pin_t pin, uint8_t source_mask);

/**
 * @brief Poll the data-ready flag (INT_STATUS_DRDY, TDK DS-000451
 *   Rev 1.0, p.67) without wiring a physical interrupt pin.
 *
 * DATA_RDY_INT (bit 0) sets when a Data Ready interrupt condition is
 * generated and clears when this register is read (R/C).  This
 * complements, it does not replace, the fixed accel/gyro start-up
 * waits in @ref icm42670_init / @ref icm42670_set_accel /
 * @ref icm42670_set_gyro -- polling this bit before the relevant
 * engine's start-up floor has elapsed is meaningless, the bit simply
 * has not been asserted yet.
 *
 * @return ALP_OK / ALP_ERR_NOT_READY (uninitialised) / ALP_ERR_INVAL
 *   (`ready` NULL).
 */
alp_status_t icm42670_data_ready(icm42670_t *dev, bool *ready);

/** Release the driver context.  Does not power down the chip. */
void icm42670_deinit(icm42670_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_ICM42670_H */
