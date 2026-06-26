/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file semtech_sx1276.h
 * @brief Semtech SX1276 legacy LoRa transceiver (SPI).
 *
 * Ubiquitous earlier-generation LoRa transceiver -- still the
 * dominant chip in deployed LoRa products.  Different register
 * map vs. SX1262 (linear, not opcode-based).
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl] — register R/W + version read.
 *
 * Datasheet: Semtech SX1276/77/78/79 v7 (Apr 2020).
 */

#ifndef ALP_CHIPS_SEMTECH_SX1276_H
#define ALP_CHIPS_SEMTECH_SX1276_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SX1276_REG_VERSION 0x42u /**< Silicon-version register address. */
#define SX1276_VERSION     0x12u /**< Datasheet-reported silicon version. */

/** @brief Driver context for one SX1276 transceiver. Caller-allocated; populated
 *  by @ref semtech_sx1276_init. Not thread-safe: serialise access per instance. */
typedef struct {
	alp_spi_t  *bus;         /**< Caller-opened SPI bus (not owned). */
	alp_gpio_t *nreset;      /**< Active-low hardware reset line (not owned). */
	bool        initialised; /**< True once @ref semtech_sx1276_init succeeds. */
} semtech_sx1276_t;

/** @brief Bind context to caller-opened SPI + reset GPIO.
 *  @param dev    Driver context (output); caller-allocated.
 *  @param spi    Caller-opened SPI bus (not owned, must outlive @p dev).
 *  @param nreset Active-low reset GPIO (not owned).
 *  @return ALP_OK on success, ALP_ERR_INVAL on NULL args. */
alp_status_t semtech_sx1276_init(semtech_sx1276_t *dev, alp_spi_t *spi, alp_gpio_t *nreset);

/** @brief Pulse NRESET (low for 100 us, then wait 5 ms).
 *  @param dev Initialised driver context.
 *  @return ALP_OK on success, ALP_ERR_IO on GPIO error. */
alp_status_t semtech_sx1276_hw_reset(semtech_sx1276_t *dev);

/** @brief Read REG_VERSION (should equal @ref SX1276_VERSION).
 *  @param dev     Initialised driver context.
 *  @param ver_out [out] version byte read from the chip.
 *  @return ALP_OK on success, ALP_ERR_IO on SPI error. */
alp_status_t semtech_sx1276_read_version(semtech_sx1276_t *dev, uint8_t *ver_out);

/** @brief Read register at @p reg.
 *  @param dev Initialised driver context.
 *  @param reg Register address.
 *  @param val [out] register value read.
 *  @return ALP_OK on success, ALP_ERR_IO on SPI error. */
alp_status_t semtech_sx1276_read_reg(semtech_sx1276_t *dev, uint8_t reg, uint8_t *val);

/** @brief Write @p val to register @p reg.
 *  @param dev Initialised driver context.
 *  @param reg Register address.
 *  @param val Value to write.
 *  @return ALP_OK on success, ALP_ERR_IO on SPI error. */
alp_status_t semtech_sx1276_write_reg(semtech_sx1276_t *dev, uint8_t reg, uint8_t val);

/** @brief Release driver context.  Idempotent.
 *  @param dev Driver context (may be NULL, in which case the call is a no-op). */
void semtech_sx1276_deinit(semtech_sx1276_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_SEMTECH_SX1276_H */
