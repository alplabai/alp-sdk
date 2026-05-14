/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file gdew0154t8.h
 * @brief GoodDisplay GDEW0154T8 1.54" monochrome e-paper module (SPI).
 *
 * 200 × 200 monochrome e-paper module common on the M5Stack Core
 * Ink and ESP32 e-paper carriers.  4-wire SPI plus D/C# / RESET /
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

#define GDEW0154T8_WIDTH  200
#define GDEW0154T8_HEIGHT 200

typedef struct {
    alp_spi_t  *bus;
    alp_gpio_t *dc;
    alp_gpio_t *reset;
    alp_gpio_t *busy;
    bool        initialised;
} gdew0154t8_t;

/** @brief Initialise the panel.  Mirrors `il3820_init`. */
alp_status_t gdew0154t8_init(gdew0154t8_t *dev,
                             alp_spi_t    *spi,
                             alp_gpio_t   *dc,
                             alp_gpio_t   *reset,
                             alp_gpio_t   *busy);

/** @brief Wait for BUSY to de-assert. */
alp_status_t gdew0154t8_wait_idle(gdew0154t8_t *dev, uint32_t timeout_ms);

/** @brief Push a single command byte. */
alp_status_t gdew0154t8_write_cmd(gdew0154t8_t *dev, uint8_t cmd);

/** @brief Push a data buffer. */
alp_status_t gdew0154t8_write_data(gdew0154t8_t *dev, const uint8_t *data, size_t len);

/** @brief Pulse the RESET pin. */
alp_status_t gdew0154t8_hw_reset(gdew0154t8_t *dev);

/** @brief Release the driver context. */
void gdew0154t8_deinit(gdew0154t8_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_GDEW0154T8_H */
