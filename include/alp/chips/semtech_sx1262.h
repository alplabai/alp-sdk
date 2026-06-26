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

#define SX1262_OPCODE_GET_STATUS 0xC0u /**< GetStatus command opcode. */
#define SX1262_OPCODE_RESET      0x82u /**< Software-reset command opcode. */
#define SX1262_OPCODE_WAKEUP     0xC0u /**< Wake-from-sleep opcode (shares GetStatus value). */

/** @brief Driver context for one SX1262 transceiver. Caller-allocated; populated
 *  by @ref semtech_sx1262_init. Not thread-safe: serialise access per instance. */
typedef struct {
	alp_spi_t  *bus;         /**< Caller-opened SPI bus (not owned). */
	alp_gpio_t *nreset;      /**< Active-low hardware reset line (not owned). */
	alp_gpio_t *busy;        /**< BUSY status input; high while the chip is busy (not owned). */
	bool        initialised; /**< True once @ref semtech_sx1262_init succeeds. */
} semtech_sx1262_t;

/** @brief Bind context to caller-opened SPI + GPIO.
 *  @param dev    Driver context (output); caller-allocated.
 *  @param spi    Caller-opened SPI bus (not owned, must outlive @p dev).
 *  @param nreset Active-low reset GPIO (not owned).
 *  @param busy   BUSY status GPIO (not owned).
 *  @return ALP_OK on success, ALP_ERR_INVAL on NULL args. */
alp_status_t
semtech_sx1262_init(semtech_sx1262_t *dev, alp_spi_t *spi, alp_gpio_t *nreset, alp_gpio_t *busy);

/** @brief Pulse NRESET for at least 100 us.
 *  @param dev Initialised driver context.
 *  @return ALP_OK on success, ALP_ERR_IO on GPIO error. */
alp_status_t semtech_sx1262_hw_reset(semtech_sx1262_t *dev);

/** @brief Block until BUSY pin de-asserts or timeout.
 *  @param dev        Initialised driver context.
 *  @param timeout_ms Maximum time to wait, in milliseconds.
 *  @return ALP_OK once BUSY is low, ALP_ERR_TIMEOUT if it stays high past @p timeout_ms. */
alp_status_t semtech_sx1262_wait_busy(semtech_sx1262_t *dev, uint32_t timeout_ms);

/** @brief Issue GetStatus and return the 8-bit status byte.
 *  @param dev        Initialised driver context.
 *  @param status_out [out] raw status byte (chip mode + command status fields).
 *  @return ALP_OK on success, ALP_ERR_IO on SPI error. */
alp_status_t semtech_sx1262_get_status(semtech_sx1262_t *dev, uint8_t *status_out);

/** @brief Release driver context.  Idempotent.
 *  @param dev Driver context (may be NULL, in which case the call is a no-op). */
void semtech_sx1262_deinit(semtech_sx1262_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_SEMTECH_SX1262_H */
