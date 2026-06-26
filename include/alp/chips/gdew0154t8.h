/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file gdew0154t8.h
 * @brief GoodDisplay GDEW0154T8 1.54" monochrome e-paper module (SPI).
 *
 * 200 × 200 monochrome e-paper module common on the M5Stack Core
 * Ink and ESP32 e-paper boards.  4-wire SPI plus D/C# / RESET /
 * BUSY ancillaries.  Same overall shape as `il3820` — different
 * panel geometry and refresh waveform.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl] — init + busy-wait + soft reset.
 *
 * Reference: GoodDisplay GDEW0154T8 module datasheet (Rev 1.0).
 */

#ifndef ALP_CHIPS_GDEW0154T8_H
#define ALP_CHIPS_GDEW0154T8_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

#define GDEW0154T8_WIDTH  200 /**< Panel width in pixels. */
#define GDEW0154T8_HEIGHT 200 /**< Panel height in pixels. */

/** Driver context.  Caller-allocated; bus + GPIO handles are borrowed
 *  (the caller owns and must keep them alive for the device's lifetime). */
typedef struct {
	alp_spi_t  *bus;         /**< 4-wire SPI bus handle. */
	alp_gpio_t *dc;          /**< D/C# select: low = command, high = data. */
	alp_gpio_t *reset;       /**< Hardware RESET line (active low). */
	alp_gpio_t *busy;        /**< BUSY input from the panel (active high). */
	bool        initialised; /**< Set once @ref gdew0154t8_init succeeds. */
} gdew0154t8_t;

/**
 * @brief Initialise the panel.  Mirrors `il3820_init`.
 *
 * @param dev    Caller-allocated context to populate.
 * @param spi    SPI bus handle (borrowed).
 * @param dc     D/C# GPIO (borrowed).
 * @param reset  RESET GPIO, active low (borrowed).
 * @param busy   BUSY GPIO input (borrowed).
 * @return @ref ALP_OK on success, @ref ALP_ERR_INVAL on NULL args, else
 *         the underlying bus/GPIO error.
 */
alp_status_t gdew0154t8_init(
    gdew0154t8_t *dev, alp_spi_t *spi, alp_gpio_t *dc, alp_gpio_t *reset, alp_gpio_t *busy);

/**
 * @brief Wait for BUSY to de-assert (panel idle).
 *
 * @param dev         Initialised context.
 * @param timeout_ms  Maximum wait in milliseconds.
 * @return @ref ALP_OK once idle, @ref ALP_ERR_TIMEOUT if BUSY stays
 *         asserted, or @ref ALP_ERR_INVAL on NULL/uninitialised dev.
 */
alp_status_t gdew0154t8_wait_idle(gdew0154t8_t *dev, uint32_t timeout_ms);

/**
 * @brief Push a single command byte (D/C# driven low).
 *
 * @param dev  Initialised context.
 * @param cmd  Controller command opcode.
 * @return @ref ALP_OK on success, else a bus/GPIO error.
 */
alp_status_t gdew0154t8_write_cmd(gdew0154t8_t *dev, uint8_t cmd);

/**
 * @brief Push a data buffer (D/C# driven high).
 *
 * @param dev   Initialised context.
 * @param data  Bytes to send (must not be NULL when @p len > 0).
 * @param len   Byte count.
 * @return @ref ALP_OK on success, else a bus/GPIO error.
 */
alp_status_t gdew0154t8_write_data(gdew0154t8_t *dev, const uint8_t *data, size_t len);

/**
 * @brief Pulse the RESET pin to hardware-reset the controller.
 *
 * @param dev  Initialised context.
 * @return @ref ALP_OK on success, else a GPIO error.
 */
alp_status_t gdew0154t8_hw_reset(gdew0154t8_t *dev);

/**
 * @brief Release the driver context.  Does NOT close the bus/GPIO handles.
 *
 * @param dev  Context to tear down (NULL is tolerated).
 */
void gdew0154t8_deinit(gdew0154t8_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_GDEW0154T8_H */
