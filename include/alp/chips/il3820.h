/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file il3820.h
 * @brief Solomon IL3820 4.2" tri-colour e-paper driver (SPI).
 *
 * Driver for the common 4.2" e-paper module (296 × 128) — supports
 * black, white, and red ink layers.  4-wire SPI, D/C# command/data
 * select, plus BUSY input from the panel.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl] — init + busy-wait + soft reset.
 *   Waveform LUTs + full-refresh sequencing land once the maintainer
 *   sources the manufacturer's reference waveform table.
 *
 * Datasheet: Solomon IL3820 (no public datasheet; pattern follows
 * GoodDisplay GDEW042Z15 module-vendor reference code).
 */

#ifndef ALP_CHIPS_IL3820_H
#define ALP_CHIPS_IL3820_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define IL3820_WIDTH 400
#define IL3820_HEIGHT 300

typedef struct {
	alp_spi_t  *bus;
	alp_gpio_t *dc;
	alp_gpio_t *reset;
	alp_gpio_t *busy;
	bool        initialised;
} il3820_t;

/** @brief Initialise an IL3820 e-paper controller over SPI. */
alp_status_t il3820_init(il3820_t *dev, alp_spi_t *spi, alp_gpio_t *dc, alp_gpio_t *reset,
                         alp_gpio_t *busy);

/** @brief Wait until BUSY de-asserts (panel ready) or timeout fires. */
alp_status_t il3820_wait_idle(il3820_t *dev, uint32_t timeout_ms);

/** @brief Push a single command byte. */
alp_status_t il3820_write_cmd(il3820_t *dev, uint8_t cmd);

/** @brief Push a data buffer. */
alp_status_t il3820_write_data(il3820_t *dev, const uint8_t *data, size_t len);

/** @brief Issue a hardware reset (pulses the RESET pin). */
alp_status_t il3820_hw_reset(il3820_t *dev);

/** @brief Release driver context. */
void il3820_deinit(il3820_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_IL3820_H */
