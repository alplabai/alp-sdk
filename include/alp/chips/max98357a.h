/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file max98357a.h
 * @brief ADI (Maxim) MAX98357A 3 W class-D mono amplifier (I²S).
 *
 * Single-pin "shutdown" board-board class-D amp.  No I²C config
 * surface -- gain is set by a single resistor / strap pin.  This
 * driver controls the shutdown GPIO + the I²S handle is owned by
 * the consumer.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes
 *   NULL-arg smokes; no HiL silicon bring-up yet.  Treat all numbers
 *   + lifecycle sequencing as paper-correct only until the v1.0
 *   verification sweep lands.
 *
 * Datasheet: ADI (Maxim) MAX98357A (Rev 5, Mar 2020).
 */

#ifndef ALP_CHIPS_MAX98357A_H
#define ALP_CHIPS_MAX98357A_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Driver context for one MAX98357A. Treat as opaque; populated by
 *        @ref max98357a_init. The I2S data stream is owned entirely by the
 *        consumer; this driver only manages the SD_MODE pin.
 */
typedef struct {
	alp_gpio_t *sd_mode;     /**< Borrowed active-high shutdown / mode-select pin. */
	bool        initialised; /**< True once @ref max98357a_init has run. */
} max98357a_t;

/**
 * @brief Bind a context to a caller-opened SD_MODE GPIO.
 *
 * Does not drive the pin; the amp's enable state after init is whatever the
 * GPIO was left in. Call @ref max98357a_set_enabled to take control.
 *
 * @param dev     Context to initialise. Must be non-NULL.
 * @param sd_mode Open GPIO wired to SD_MODE; borrowed, must outlive @p dev.
 * @return ALP_OK on success, or an error if arguments are invalid.
 */
alp_status_t max98357a_init(max98357a_t *dev, alp_gpio_t *sd_mode);

/**
 * @brief Enable or shut down the amplifier via SD_MODE.
 *
 * Drives SD_MODE high to enable the amp, low to shut it down (zero quiescent
 * current). Toggling does not affect the I2S clock/data lines.
 *
 * @param dev     Initialised context.
 * @param enabled True to enable output, false to shut down.
 * @return ALP_OK on success, or an error from the GPIO write.
 */
alp_status_t max98357a_set_enabled(max98357a_t *dev, bool enabled);

/**
 * @brief Release the driver context. Does not change the SD_MODE pin state and
 *        does not release the borrowed GPIO.
 *
 * @param dev Context to release; NULL is ignored.
 */
void max98357a_deinit(max98357a_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_MAX98357A_H */
