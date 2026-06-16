/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ra8875.h
 * @brief RAiO RA8875 LCD controller + resistive-touch IC (SPI).
 *
 * Embedded LCD controller for 5–7" panels with on-chip framebuffer,
 * BTE 2D acceleration, and an integrated 5-wire resistive-touch
 * controller.  4-wire SPI; D/C select packed into the SPI command
 * byte (no separate D/C# pin like ILI9xxx).
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 * @par Driver status: [stub-impl] — chip-ID probe + soft reset.
 *   Panel-init / BTE / touch lifecycle land once the maintainer
 *   adds the RA8875 panel-init table to the design archive.
 *
 * Datasheet: RAiO RA8875 v1.7 (Mar 2014).
 */

#ifndef ALP_CHIPS_RA8875_H
#define ALP_CHIPS_RA8875_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** RA8875 SPI command bytes (data direction + register/data flag). */
#define RA8875_CMD_WRITE  0x80u
#define RA8875_CMD_READ   0xC0u
#define RA8875_DATA_WRITE 0x00u
#define RA8875_DATA_READ  0x40u

#define RA8875_REG_PWRR 0x01u /**< Power & Display Control Register. */

typedef struct {
	alp_spi_t  *bus;
	alp_gpio_t *reset;
	bool        initialised;
} ra8875_t;

/**
 * @brief Initialise an RA8875 over SPI.
 *
 * Toggles hardware reset (if pin supplied), then sets PWRR to
 * "display on".  Panel-specific PLL + LCD-clock + horizontal/
 * vertical-timing registers must be configured by a follow-up call
 * once the panel datasheet is mapped — left to `[stub-impl]` for now.
 */
alp_status_t ra8875_init(ra8875_t *dev, alp_spi_t *spi, alp_gpio_t *reset);

/** @brief Read register at @p reg over SPI. */
alp_status_t ra8875_read_reg(ra8875_t *dev, uint8_t reg, uint8_t *val);

/** @brief Write @p val to register @p reg over SPI. */
alp_status_t ra8875_write_reg(ra8875_t *dev, uint8_t reg, uint8_t val);

/** @brief Issue software reset (PWRR bit 0). */
alp_status_t ra8875_soft_reset(ra8875_t *dev);

/** @brief Release the driver context.  NULL tolerated. */
void ra8875_deinit(ra8875_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_RA8875_H */
