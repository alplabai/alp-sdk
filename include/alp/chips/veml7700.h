/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file veml7700.h
 * @brief Vishay VEML7700 high-precision ambient-light sensor (I²C).
 *
 * 16-bit lux-output ALS with on-chip gain + integration controls.
 * Less complex than TSL2591 (single visible-light channel) -- one
 * 16-bit count register maps directly to lux via a configurable
 * scale.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: Vishay VEML7700 Rev 1.5 (May 2020).
 */

#ifndef ALP_CHIPS_VEML7700_H
#define ALP_CHIPS_VEML7700_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VEML7700_I2C_ADDR 0x10u /**< Fixed 7-bit I²C address (the part has no address pin). */

#define VEML7700_REG_CONF       0x00u /**< Config: CONF[0]=SD shutdown, plus gain / IT bits. */
#define VEML7700_REG_HIGH_TH    0x01u /**< High interrupt threshold (16-bit). */
#define VEML7700_REG_LOW_TH     0x02u /**< Low interrupt threshold (16-bit). */
#define VEML7700_REG_POWER_SAVE 0x03u /**< Power-saving mode register. */
#define VEML7700_REG_ALS        0x04u /**< ALS output counts (16-bit, lux after scaling). */
#define VEML7700_REG_WHITE      0x05u /**< White-channel output counts (16-bit). */

/**
 * @brief Driver context for one VEML7700 device.
 *
 * Caller-allocated and owned; bind it with veml7700_init() and release it with
 * veml7700_deinit(). Treat fields as read-only. Not thread-safe: serialise calls
 * that share one context (and the underlying I²C bus) externally.
 */
typedef struct {
	alp_i2c_t *bus;         /**< Borrowed I²C bus handle; not closed by veml7700_deinit(). */
	uint8_t    addr;        /**< 7-bit I²C address in use (normally @ref VEML7700_I2C_ADDR). */
	bool       initialised; /**< True once veml7700_init() succeeds; gates read/deinit. */
} veml7700_t;

/**
 * @brief Bind the context to an I²C bus and power on the ALS.
 *
 * Writes the config register with CONF[0] (shutdown) cleared, leaving gain and
 * integration time at their power-on defaults. Does not read or verify a device ID
 * (the VEML7700 exposes none).
 *
 * @param dev       Caller-allocated context to populate. Must be non-NULL.
 * @param bus       Open, caller-owned I²C bus handle; borrowed for the device
 *                  lifetime (not released by veml7700_deinit()).
 * @param i2c_addr  7-bit address, normally @ref VEML7700_I2C_ADDR. Must be non-zero.
 * @return @ref ALP_OK on success; @ref ALP_ERR_INVAL on a NULL arg or zero
 *         address; or the bus error from the config write.
 */
alp_status_t veml7700_init(veml7700_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Read the raw 16-bit ALS counts.
 *
 * Returns the unscaled ALS register value; convert to lux in the caller using the
 * configured gain and integration time.
 *
 * @param dev      Initialised context.
 * @param als_out  Output: 16-bit ALS count. Must be non-NULL.
 * @return @ref ALP_OK on success; @ref ALP_ERR_NOT_READY if not initialised;
 *         @ref ALP_ERR_INVAL on a NULL output pointer; or the underlying bus error.
 */
alp_status_t veml7700_read_als(veml7700_t *dev, uint16_t *als_out);

/**
 * @brief Release the driver context.
 *
 * Clears the bound bus and the initialised flag. Does not power down the sensor
 * or close the borrowed I²C bus. Safe to call with @p dev NULL (no-op).
 *
 * @param dev  Context to release; may be NULL.
 */
void veml7700_deinit(veml7700_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_VEML7700_H */
