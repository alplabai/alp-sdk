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
 * Per TI SBOS473L (TMP112 family datasheet, rev. Jul 2024), Table 7-4,
 * p.16, the 7-bit I2C address depends on BOTH the ADD0 strap AND the
 * orderable variant -- the table lists three rows, not two, and package
 * alone does not tell them apart (X2SON-5 covers two of the three):
 *   - Address Variant Only, X2SON-5 package (TMP112D):
 *       ADD0 strapped; one physical part, address set by the strap.
 *       0x40  ADD0 = GND
 *       0x41  ADD0 = V+
 *       0x42  ADD0 = SDA
 *       0x43  ADD0 = SCL
 *   - Alert Variant Only, X2SON-5 package (TMP112D0/D1/D2/D3):
 *       ADD0 = N/A -- no strap pin; the address is fixed per orderable
 *       part number, not selected in-circuit.
 *       0x48  TMP112D0
 *       0x49  TMP112D1
 *       0x4A  TMP112D2
 *       0x4B  TMP112D3
 *   - Address + Alert Variant, SOT563-6 package (TMP112A/B/D/N):
 *       ADD0 strapped; one physical part, address set by the strap.
 *       0x48  ADD0 = GND
 *       0x49  ADD0 = V+
 *       0x4A  ADD0 = SDA
 *       0x4B  ADD0 = SCL
 *   Row 2 and row 3 share the 0x48..0x4B address range by design -- an
 *   Alert-Variant-Only part and a strapped Address+Alert part can read
 *   the same 7-bit address; they are not interchangeable in-circuit,
 *   only numerically identical.  All three rows are accepted by this
 *   driver; which part is fitted on a given board is a per-SoM metadata
 *   question, not a driver one.
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

/**
 * Address + Alert variant, SOT563-6 package, ADD0-strapped
 * (SBOS473L Table 7-4, p.16).  Numerically identical to the
 * Alert-Variant-Only X2SON-5 parts' fixed addresses below --
 * 0x48..0x4B is shared by both rows of the table, not unique to
 * this package.
 */
#define TMP112_I2C_ADDR_GND   0x48u
#define TMP112_I2C_ADDR_VPLUS 0x49u
#define TMP112_I2C_ADDR_SDA   0x4Au
#define TMP112_I2C_ADDR_SCL   0x4Bu

/**
 * Address Variant Only, X2SON-5 package, TMP112D, ADD0-strapped
 * (SBOS473L Table 7-4, p.16).
 */
#define TMP112_I2C_ADDR_ADDRVAR_GND   0x40u
#define TMP112_I2C_ADDR_ADDRVAR_VPLUS 0x41u
#define TMP112_I2C_ADDR_ADDRVAR_SDA   0x42u
#define TMP112_I2C_ADDR_ADDRVAR_SCL   0x43u

/** Conversion-rate enum (CR1:CR0 in CONF). */
typedef enum {
	TMP112_RATE_0_25_HZ = 0,
	TMP112_RATE_1_HZ    = 1,
	TMP112_RATE_4_HZ    = 2, /**< Datasheet default. */
	TMP112_RATE_8_HZ    = 3,
} tmp112_rate_t;

typedef struct {
	bool       initialised;
	alp_i2c_t *bus;
	uint8_t    addr;
	bool       extended_mode; /**< 13-bit if true (extended range up to +150 C). */
} tmp112_t;

/** @brief Probe + configure for continuous-conversion mode. */
alp_status_t tmp112_init(tmp112_t *ctx, alp_i2c_t *bus, uint8_t addr_7bit);

/** @brief Set conversion rate.  Default is 4 Hz. */
alp_status_t tmp112_set_rate(tmp112_t *ctx, tmp112_rate_t rate);

/** @brief Toggle 13-bit extended-range mode (default off / 12-bit). */
alp_status_t tmp112_set_extended_mode(tmp112_t *ctx, bool extended);

/**
 * @brief Read temperature in milli-degrees Celsius (1/1000 C).
 *
 * Avoids float on M-class targets.  Caller divides by 1000 for
 * degrees C.  Resolution: 0.0625 C * 1000 = 62.5 milli-C/LSB.
 */
alp_status_t tmp112_read_temp_milli_c(tmp112_t *ctx, int32_t *temp_milli_c);

/** @brief Release resources.  Idempotent. */
void tmp112_deinit(tmp112_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_TMP112_H */
