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

#define IL3820_WIDTH  400 /**< Controller addressable width in pixels (source-driver span). */
#define IL3820_HEIGHT 300 /**< Controller addressable height in pixels (gate-driver span). */

/** @brief Driver context.  Caller-allocated; treat fields as opaque. */
typedef struct {
	alp_spi_t  *bus;         /**< Bound SPI bus (borrowed; not owned/closed by driver). */
	alp_gpio_t *dc;          /**< D/C# select: low = command, high = data. */
	alp_gpio_t *reset;       /**< Active-low hardware RESET line. */
	alp_gpio_t *busy;        /**< BUSY input from panel; high while refreshing. */
	bool        initialised; /**< True once il3820_init() has run. */
} il3820_t;

/**
 * @brief Initialise an IL3820 e-paper controller over SPI.
 *
 * Binds the bus and control lines and issues the power-on reset sequence.
 * All GPIO/SPI handles are borrowed and must outlive @p dev.
 *
 * @param dev    Caller-allocated context to initialise.
 * @param spi    Open SPI bus.
 * @param dc     D/C# command/data select GPIO.
 * @param reset  Active-low RESET GPIO.
 * @param busy   BUSY status-input GPIO.
 * @return ALP_OK on success; ALP_ERR_IO on bus/GPIO failure.
 */
alp_status_t
il3820_init(il3820_t *dev, alp_spi_t *spi, alp_gpio_t *dc, alp_gpio_t *reset, alp_gpio_t *busy);

/**
 * @brief Wait until BUSY de-asserts (panel ready) or the timeout fires.
 *
 * @param dev         Initialised context.
 * @param timeout_ms  Maximum wait in milliseconds.
 * @return ALP_OK once idle; ALP_ERR_TIMEOUT if BUSY stays asserted.
 */
alp_status_t il3820_wait_idle(il3820_t *dev, uint32_t timeout_ms);

/**
 * @brief Push a single command byte (D/C# driven low).
 *
 * @param dev  Initialised context.
 * @param cmd  Command opcode.
 * @return ALP_OK on success; ALP_ERR_IO on bus failure.
 */
alp_status_t il3820_write_cmd(il3820_t *dev, uint8_t cmd);

/**
 * @brief Push a data buffer (D/C# driven high).
 *
 * @param dev   Initialised context.
 * @param data  Bytes to send; borrowed for the call only.
 * @param len   Number of bytes in @p data.
 * @return ALP_OK on success; ALP_ERR_IO on bus failure.
 */
alp_status_t il3820_write_data(il3820_t *dev, const uint8_t *data, size_t len);

/**
 * @brief Issue a hardware reset (pulses the RESET pin).
 *
 * @param dev  Initialised context.
 * @return ALP_OK on success; ALP_ERR_IO on GPIO failure.
 */
alp_status_t il3820_hw_reset(il3820_t *dev);

/**
 * @brief Release driver context.
 *
 * Clears the initialised flag; does not power down the panel or close the
 * borrowed SPI/GPIO handles.
 *
 * @param dev  Context to release (may be NULL).
 */
void il3820_deinit(il3820_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_IL3820_H */
