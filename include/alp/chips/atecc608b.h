/*
 * Copyright 2026 ALP Lab AB
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

#define ATECC608B_I2C_ADDR_DEFAULT 0x35u

typedef struct {
    alp_i2c_t *bus;
    uint8_t    addr;
    bool       initialised;
} atecc608b_t;

/** @brief Bind context to caller-opened I²C bus. */
alp_status_t atecc608b_init(atecc608b_t *dev, alp_i2c_t *bus, uint8_t i2c_addr);

/** @brief Send the wake sequence (low SDA for >60us, then read). */
alp_status_t atecc608b_wake(atecc608b_t *dev);

/** @brief Send the idle command (0x02). */
alp_status_t atecc608b_idle(atecc608b_t *dev);

/** @brief Send the sleep command (0x01). */
alp_status_t atecc608b_sleep(atecc608b_t *dev);

/** @brief Release driver context. */
void atecc608b_deinit(atecc608b_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_ATECC608B_H */
