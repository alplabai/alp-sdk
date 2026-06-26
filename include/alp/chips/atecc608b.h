/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file atecc608b.h
 * @brief Microchip ATECC608B secure element (I²C).
 *
 * EC P-256 + AES-128 hardware crypto + secure key storage in a
 * 1-mm² SOT-23.  Speaks Microchip's proprietary "ATCA" packet
 * protocol over I²C.  This driver covers chip probe + the
 * `wake` / `idle` / `sleep` lifecycle commands.  Crypto-engine
 * sub-commands (sign / verify / nonce / random) land alongside
 * the Microchip CryptoAuthLib import.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl]
 *
 * Datasheet: Microchip ATECC608B (Rev D, Aug 2021).
 */

#ifndef ALP_CHIPS_ATECC608B_H
#define ALP_CHIPS_ATECC608B_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ATECC608B_I2C_ADDR_DEFAULT 0x35u /**< Factory-default 7-bit I2C address. */

/**
 * @brief ATECC608B driver context (I2C).
 *
 * @p bus is borrowed, not owned -- it must outlive the context.
 */
typedef struct {
	alp_i2c_t *bus;         /**< Caller-opened I2C bus handle (borrowed). */
	uint8_t    addr;        /**< Active 7-bit slave address (ATECC608B_I2C_ADDR_DEFAULT). */
	bool       initialised; /**< True between a successful init and deinit. */
} atecc608b_t;

/**
 * @brief Bind the context to a caller-opened I2C bus.
 *
 * The bus is borrowed and must stay valid until atecc608b_deinit().  The device
 * powers up in sleep; call atecc608b_wake() before issuing commands.
 *
 * @param dev       Context to initialise (output).
 * @param bus       Caller-opened I2C bus handle.
 * @param i2c_addr  7-bit slave address (ATECC608B_I2C_ADDR_DEFAULT unless reprovisioned).
 * @return          ALP_OK on success, ALP_ERR_INVAL on NULL args.
 */
alp_status_t atecc608b_init(atecc608b_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/**
 * @brief Send the wake sequence (hold SDA low > 60 us, then read).
 *
 * Transitions the device from sleep/idle to active so it can accept commands.
 *
 * @param dev  Initialised context.
 * @return     ALP_OK on success, ALP_ERR_NOT_READY if not initialised,
 *             ALP_ERR_INVAL on NULL @p dev.
 */
alp_status_t atecc608b_wake(atecc608b_t *dev);

/**
 * @brief Send the idle command (0x02).
 *
 * Parks the device in idle: lower power than active while retaining volatile
 * state (TempKey, nonce); the next command needs no fresh wake.
 *
 * @param dev  Initialised context.
 * @return     ALP_OK on success, ALP_ERR_NOT_READY if not initialised,
 *             ALP_ERR_INVAL on NULL @p dev.
 */
alp_status_t atecc608b_idle(atecc608b_t *dev);

/**
 * @brief Send the sleep command (0x01).
 *
 * Enters the lowest-power state and clears volatile state; a subsequent
 * atecc608b_wake() is required before the next command.
 *
 * @param dev  Initialised context.
 * @return     ALP_OK on success, ALP_ERR_NOT_READY if not initialised,
 *             ALP_ERR_INVAL on NULL @p dev.
 */
alp_status_t atecc608b_sleep(atecc608b_t *dev);

/**
 * @brief Release the driver context.  Idempotent; NULL tolerated.
 *
 * Does not close the borrowed bus handle -- the caller owns it.
 *
 * @param dev  Context to release (may be NULL).
 */
void atecc608b_deinit(atecc608b_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_ATECC608B_H */
