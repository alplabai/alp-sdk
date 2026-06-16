/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file semtech_sx1262.h
 * @brief Semtech SX1262 LoRa transceiver (SPI).
 *
 * Sub-GHz LoRa/FSK transceiver -- the newer SX126x family that
 * supersedes SX1276 in new designs.  Driver covers register read /
 * write + a `GetStatus` probe; LoRa packet TX/RX layer lives in the
 * (LoRaWAN MAC-layer) `LoRaMac-node` library.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl] — SPI shell + status probe.
 *
 * Datasheet: Semtech SX1262 v1.2 (Jun 2019).
 */

#ifndef ALP_CHIPS_SEMTECH_SX1262_H
#define ALP_CHIPS_SEMTECH_SX1262_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SX1262_OPCODE_GET_STATUS 0xC0u
#define SX1262_OPCODE_RESET 0x82u
#define SX1262_OPCODE_WAKEUP 0xC0u

typedef struct {
	alp_spi_t  *bus;
	alp_gpio_t *nreset;
	alp_gpio_t *busy;
	bool        initialised;
} semtech_sx1262_t;

/** @brief Bind context to caller-opened SPI + GPIO. */
alp_status_t semtech_sx1262_init(semtech_sx1262_t *dev, alp_spi_t *spi, alp_gpio_t *nreset,
                                 alp_gpio_t *busy);

/** @brief Pulse NRESET for at least 100 us. */
alp_status_t semtech_sx1262_hw_reset(semtech_sx1262_t *dev);

/** @brief Block until BUSY pin de-asserts or timeout. */
alp_status_t semtech_sx1262_wait_busy(semtech_sx1262_t *dev, uint32_t timeout_ms);

/** @brief Issue GetStatus and return the 8-bit status byte. */
alp_status_t semtech_sx1262_get_status(semtech_sx1262_t *dev, uint8_t *status_out);

/** @brief Release driver context. */
void semtech_sx1262_deinit(semtech_sx1262_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_SEMTECH_SX1262_H */
