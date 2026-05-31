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

#define SX1276_REG_VERSION 0x42u
#define SX1276_VERSION 0x12u /**< Datasheet-reported silicon version. */

typedef struct {
    alp_spi_t  *bus;
    alp_gpio_t *nreset;
    bool        initialised;
} semtech_sx1276_t;

/** @brief Bind context to caller-opened SPI + reset GPIO. */
alp_status_t semtech_sx1276_init(semtech_sx1276_t *dev, alp_spi_t *spi, alp_gpio_t *nreset);

/** @brief Pulse NRESET (low for 100 us, then wait 5 ms). */
alp_status_t semtech_sx1276_hw_reset(semtech_sx1276_t *dev);

/** @brief Read REG_VERSION (should equal @ref SX1276_VERSION). */
alp_status_t semtech_sx1276_read_version(semtech_sx1276_t *dev, uint8_t *ver_out);

/** @brief Read register at @p reg. */
alp_status_t semtech_sx1276_read_reg(semtech_sx1276_t *dev, uint8_t reg, uint8_t *val);

/** @brief Write @p val to register @p reg. */
alp_status_t semtech_sx1276_write_reg(semtech_sx1276_t *dev, uint8_t reg, uint8_t val);

/** @brief Release driver context. */
void semtech_sx1276_deinit(semtech_sx1276_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_SEMTECH_SX1276_H */
