/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tmp112.h
 * @brief TI TMP112 digital temperature sensor driver.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * 12/13-bit I2C temperature sensor, +/-0.5 C accuracy from
 * -25..+85 C, 0.0625 C/LSB resolution.  On the E1M-AEN module
 * the sensor sits on Alif's LPI2C bus alongside the RV-3028-C7
 * RTC and the OPTIGA Trust M secure element.
 *
 * Per TMP112 datasheet (SBOS473K):
 *   - 7-bit I2C address depends on ADD0 strap
 *       0x48  ADD0 = GND     (default on E1M-AEN)
 *       0x49  ADD0 = V+
 *       0x4A  ADD0 = SDA
 *       0x4B  ADD0 = SCL
 *   - Registers
 *       0x00  Temperature   (RO, 12/13-bit signed, 0.0625 C/LSB)
 *       0x01  Configuration (RW, see datasheet table 9)
 *       0x02  T_LOW         (RW)
 *       0x03  T_HIGH        (RW)
 */

#ifndef ALP_CHIPS_TMP112_H
#define ALP_CHIPS_TMP112_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TMP112_I2C_ADDR_GND   0x48u
#define TMP112_I2C_ADDR_VPLUS 0x49u
#define TMP112_I2C_ADDR_SDA   0x4Au
#define TMP112_I2C_ADDR_SCL   0x4Bu

/** @brief Conversion-rate selector (CR1:CR0 in the CONF register). */
typedef enum {
	TMP112_RATE_0_25_HZ = 0, /**< One conversion every 4 s. */
	TMP112_RATE_1_HZ    = 1, /**< One conversion every 1 s. */
	TMP112_RATE_4_HZ    = 2, /**< Four conversions/s (datasheet default). */
	TMP112_RATE_8_HZ    = 3, /**< Eight conversions/s. */
} tmp112_rate_t;

/** @brief Driver context for one TMP112 sensor. */
typedef struct {
	bool       initialised;   /**< True once tmp112_init() probed the chip. */
	alp_i2c_t *bus;           /**< Borrowed I2C bus; not owned. */
	uint8_t    addr;          /**< 7-bit I2C slave address. */
	bool       extended_mode; /**< 13-bit if true (extended range up to +150 C). */
} tmp112_t;

/**
 * @brief Probe the sensor and configure continuous-conversion mode.
 *
 * @param ctx        Caller-allocated context; populated on success.
 * @param bus        Caller-opened I2C bus; borrowed, must outlive @p ctx.
 * @param addr_7bit  7-bit slave address (one of TMP112_I2C_ADDR_*).
 * @return ALP_OK; ALP_ERR_INVAL on NULL args; the underlying I2C error if
 *         the chip does not respond.
 */
alp_status_t tmp112_init(tmp112_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit);

/**
 * @brief Set the conversion rate (default 4 Hz).
 *
 * @param ctx   Initialised context.
 * @param rate  Conversion-rate selector.
 * @return ALP_OK; ALP_ERR_NOT_READY if uninitialised; the underlying I2C
 *         error otherwise.
 */
alp_status_t tmp112_set_rate(tmp112_t *ctx, tmp112_rate_t rate);

/**
 * @brief Toggle 13-bit extended-range mode (default off / 12-bit).
 *
 * Extended mode raises the upper measurable bound to +150 C.
 *
 * @param ctx       Initialised context.
 * @param extended  True for 13-bit extended range, false for 12-bit.
 * @return ALP_OK; ALP_ERR_NOT_READY if uninitialised; the underlying I2C
 *         error otherwise.
 */
alp_status_t tmp112_set_extended_mode(tmp112_t *ctx, bool extended);

/**
 * @brief Read temperature in milli-degrees Celsius (1/1000 C).
 *
 * Avoids float on M-class targets.  Caller divides by 1000 for
 * degrees C.  Resolution: 0.0625 C * 1000 = 62.5 milli-C/LSB.
 *
 * @param ctx           Initialised context.
 * @param temp_milli_c  Receives the signed temperature in milli-C.
 * @return ALP_OK; ALP_ERR_NOT_READY if uninitialised; ALP_ERR_INVAL if
 *         @p temp_milli_c is NULL; the underlying I2C error otherwise.
 */
alp_status_t tmp112_read_temp_milli_c(tmp112_t *ctx, int32_t *temp_milli_c);

/**
 * @brief Release resources (clears @c initialised).  Idempotent.
 *
 * Does not touch the borrowed I2C bus.  NULL tolerated.
 *
 * @param ctx  Context to release, or NULL.
 */
void tmp112_deinit(tmp112_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_TMP112_H */
