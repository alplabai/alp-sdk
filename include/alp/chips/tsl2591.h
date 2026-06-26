/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file tsl2591.h
 * @brief ams TSL2591 wide-dynamic-range ambient-light sensor (I²C).
 *
 * 600 M:1 dynamic range visible + IR light sensor.  Two 16-bit ADC
 * channels (full-spectrum + IR-only) -- visible light is the
 * difference.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: ams TSL2591 v0.7 (May 2013).
 */

#ifndef ALP_CHIPS_TSL2591_H
#define ALP_CHIPS_TSL2591_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TSL2591_I2C_ADDR 0x29u /**< Fixed 7-bit I²C address (the part has no address pin). */

#define TSL2591_CMD_BIT       0xA0u /**< OR into a reg addr: COMMAND bit + normal-operation type. */
#define TSL2591_REG_ENABLE    0x00u /**< ENABLE register (power-on / ALS-enable bits). */
#define TSL2591_REG_ID        0x12u /**< Device-ID register; reads @ref TSL2591_DEVICE_ID. */
#define TSL2591_REG_C0DATA_LO 0x14u /**< CH0 low byte; CH0 then CH1 follow as 4 LE bytes. */

#define TSL2591_DEVICE_ID 0x50u /**< Expected value of the ID register for a genuine TSL2591. */

#define TSL2591_ENABLE_POWER_ON 0x01u /**< ENABLE.PON: power the internal oscillator. */
#define TSL2591_ENABLE_AEN      0x02u /**< ENABLE.AEN: start ALS ADC integration. */

/**
 * @brief Driver context for one TSL2591 device.
 *
 * Caller-allocated and owned; bind it with tsl2591_init() and release it with
 * tsl2591_deinit(). Treat fields as read-only. Not thread-safe: serialise calls
 * that share one context (and the underlying I²C bus) externally.
 */
typedef struct {
	alp_i2c_t *bus;         /**< Borrowed I²C bus handle; not closed by tsl2591_deinit(). */
	uint8_t    addr;        /**< 7-bit I²C address in use (normally @ref TSL2591_I2C_ADDR). */
	bool       initialised; /**< True once tsl2591_init() succeeds; gates read/deinit. */
} tsl2591_t;

/**
 * @brief Bind the context to an I²C bus, verify the chip ID, and enable the ALS.
 *
 * Reads the ID register and rejects a mismatch, then powers the oscillator and
 * starts ADC integration (default gain / integration time).
 *
 * @param dev       Caller-allocated context to populate. Must be non-NULL.
 * @param bus       Open, caller-owned I²C bus handle; borrowed for the device
 *                  lifetime (not released by tsl2591_deinit()).
 * @param i2c_addr  7-bit address, normally @ref TSL2591_I2C_ADDR. Must be non-zero.
 * @return @ref ALP_OK on success; @ref ALP_ERR_INVAL on a NULL arg or zero
 *         address; @ref ALP_ERR_IO on an ID mismatch; or the bus error from the
 *         underlying I²C transfer.
 */
alp_status_t tsl2591_init(tsl2591_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read the two raw 16-bit ADC channels.
 *
 * Visible light is computed by the caller as CH0 - CH1; this driver returns the
 * raw counts only (no lux conversion).
 *
 * @param dev           Initialised context.
 * @param ch0_full_out  Output: CH0 full-spectrum (visible + IR) count. Must be non-NULL.
 * @param ch1_ir_out    Output: CH1 IR-only count. Must be non-NULL.
 * @return @ref ALP_OK on success; @ref ALP_ERR_NOT_READY if not initialised;
 *         @ref ALP_ERR_INVAL on a NULL output pointer; or the underlying bus error.
 */
alp_status_t tsl2591_read_channels(tsl2591_t *dev, uint16_t *ch0_full_out, uint16_t *ch1_ir_out);

/**
 * @brief Release the driver context.
 *
 * Clears the bound bus and the initialised flag. Does not power down the sensor
 * or close the borrowed I²C bus. Safe to call with @p dev NULL (no-op).
 *
 * @param dev  Context to release; may be NULL.
 */
void tsl2591_deinit(tsl2591_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_TSL2591_H */
