/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file drv8825.h
 * @brief TI DRV8825 bipolar stepper motor driver.
 *
 * Step/dir-driven bipolar stepper board-board control surface.
 * The driver orchestrates STEP (PWM) + DIR (GPIO) + nENBL (GPIO)
 * + the three microstep-select pins M0/M1/M2 (GPIO).  No on-chip
 * register file.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: TI DRV8825 SLVSA73F (Aug 2014).
 */

#ifndef ALP_CHIPS_DRV8825_H
#define ALP_CHIPS_DRV8825_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"
#include "alp/pwm.h" /* alp_pwm_t lives here, not in peripheral.h */

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Microstep resolution, encoded as the M2:M1:M0 pin pattern. */
typedef enum {
	DRV8825_USTEP_FULL = 0, /**< 000: full step. */
	DRV8825_USTEP_HALF = 1, /**< 001: half step. */
	DRV8825_USTEP_1_4  = 2, /**< 010: quarter step. */
	DRV8825_USTEP_1_8  = 3, /**< 011: eighth step. */
	DRV8825_USTEP_1_16 = 4, /**< 100: 1/16 step. */
	DRV8825_USTEP_1_32 = 5, /**< 101: 1/32 step (pin codes 5..7 all decode to 1/32). */
} drv8825_ustep_t;

/**
 * @brief DRV8825 driver context.
 *
 * Holds borrowed references to the caller-opened PWM/GPIO handles. The driver
 * does not take ownership: the caller keeps each handle open for the lifetime
 * of the context and closes them after drv8825_deinit().
 */
typedef struct {
	alp_pwm_t  *step;        /**< STEP pin PWM: one pulse advances one (micro)step. */
	alp_gpio_t *dir;         /**< DIR pin: level selects rotation direction. */
	alp_gpio_t *nenbl;       /**< nENBL active-low output enable. Optional (may be NULL). */
	alp_gpio_t *m0;          /**< Microstep-select bit M0. */
	alp_gpio_t *m1;          /**< Microstep-select bit M1. */
	alp_gpio_t *m2;          /**< Microstep-select bit M2. */
	bool        initialised; /**< True once drv8825_init() has bound the handles. */
} drv8825_t;

/**
 * @brief Bind a context to caller-opened PWM + GPIO handles.
 *
 * Stores the handles (does not take ownership) and marks the context
 * initialised. No bus traffic or pin drive happens here.
 *
 * @param dev    Output: caller-allocated context to populate.
 * @param step   Open PWM bound to the STEP pin (required).
 * @param dir    Open GPIO bound to the DIR pin (required).
 * @param nenbl  Open GPIO bound to nENBL, or NULL if the pin is hard-tied.
 * @param m0     Open GPIO bound to M0 (required for drv8825_set_microstep()).
 * @param m1     Open GPIO bound to M1.
 * @param m2     Open GPIO bound to M2.
 * @return `ALP_OK` on success; an `ALP_ERR_*` status on a NULL required handle.
 */
alp_status_t drv8825_init(drv8825_t  *dev,
                          alp_pwm_t  *step,
                          alp_gpio_t *dir,
                          alp_gpio_t *nenbl,
                          alp_gpio_t *m0,
                          alp_gpio_t *m1,
                          alp_gpio_t *m2);

/**
 * @brief Apply a microstep resolution by latching the M0/M1/M2 GPIOs.
 *
 * @param dev   Initialised context.
 * @param mode  Microstep resolution to select.
 * @return `ALP_OK` on success; an `ALP_ERR_*` status otherwise.
 */
alp_status_t drv8825_set_microstep(drv8825_t *dev, drv8825_ustep_t mode);

/**
 * @brief Set rotation direction by driving the DIR pin.
 *
 * @param dev      Initialised context.
 * @param forward  true drives DIR high (forward); false drives DIR low (reverse).
 * @return `ALP_OK` on success; an `ALP_ERR_*` status otherwise.
 */
alp_status_t drv8825_set_direction(drv8825_t *dev, bool forward);

/**
 * @brief Set step-pulse rate by configuring the STEP PWM frequency.
 *
 * @param dev      Initialised context.
 * @param freq_hz  Step frequency in Hz (0 = stop; capped at 250 kHz
 *                 per datasheet).
 * @return `ALP_OK` on success; an `ALP_ERR_*` status otherwise.
 */
alp_status_t drv8825_set_step_rate(drv8825_t *dev, uint32_t freq_hz);

/**
 * @brief Enable or disable the output stage via the nENBL pin.
 *
 * @param dev       Initialised context.
 * @param sleeping  true disables the output (motor coasts, holding torque
 *                  released); false enables the output.
 * @return `ALP_OK` on success; an `ALP_ERR_*` status otherwise.
 */
alp_status_t drv8825_set_sleep(drv8825_t *dev, bool sleeping);

/**
 * @brief Release the driver context.
 *
 * Clears the bound handles but does not close them — the caller owns the
 * underlying PWM/GPIO handles.
 *
 * @param dev  Context to release.
 */
void drv8825_deinit(drv8825_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_DRV8825_H */
