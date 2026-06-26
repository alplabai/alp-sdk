/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file a4988.h
 * @brief Allegro A4988 low-cost bipolar stepper motor driver.
 *
 * Step/dir-driven bipolar stepper driver board.  Same shape as
 * `drv8825` -- STEP (PWM) + DIR + nENABLE + microstep-select pins
 * (MS1/MS2/MS3).  No on-chip register file.
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: Allegro A4988 (Rev L, May 2014).
 */

#ifndef ALP_CHIPS_A4988_H
#define ALP_CHIPS_A4988_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"
#include "alp/pwm.h" /* alp_pwm_t lives here, not in peripheral.h */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Microstep resolution, encoded as the raw MS3:MS2:MS1 pin pattern.
 *
 * The enum value is the 3-bit level word driven onto {MS3, MS2, MS1}, so it
 * doubles as the GPIO latch pattern (MS1 = bit0).  Per the A4988 datasheet only
 * the four patterns below plus all-high (1/16) are defined; other combinations
 * are reserved.
 */
typedef enum {
	A4988_USTEP_FULL = 0, /**< 000: full step. */
	A4988_USTEP_HALF = 1, /**< 001: half step. */
	A4988_USTEP_1_4  = 2, /**< 010: quarter step. */
	A4988_USTEP_1_8  = 3, /**< 011: eighth step. */
	A4988_USTEP_1_16 = 7, /**< 111: sixteenth step. */
} a4988_ustep_t;

/**
 * @brief A4988 driver context: a bundle of caller-owned PWM + GPIO handles.
 *
 * The driver owns none of these handles -- it borrows them for its lifetime and
 * never opens or closes them (see a4988_init / a4988_deinit).  All pointers must
 * outlive the context.
 */
typedef struct {
	alp_pwm_t  *step;        /**< PWM channel generating the STEP pulse train. */
	alp_gpio_t *dir;         /**< DIR pin: level selects rotation direction. */
	alp_gpio_t *nenable;     /**< nENABLE pin (active-low): high tristates outputs. */
	alp_gpio_t *ms1;         /**< MS1 microstep-select pin (bit0). */
	alp_gpio_t *ms2;         /**< MS2 microstep-select pin (bit1). */
	alp_gpio_t *ms3;         /**< MS3 microstep-select pin (bit2). May be NULL on boards
	                          *   that hardwire MS3, limiting the max resolution. */
	bool        initialised; /**< True between a successful init and deinit. */
} a4988_t;

/**
 * @brief Bind a context to caller-opened PWM + GPIO handles.
 *
 * Does not drive any pin; call the setters to configure direction, microstep,
 * and step rate, then a4988_set_enable() to energise the outputs.  The handles
 * are borrowed, not owned -- they must remain valid until a4988_deinit().
 *
 * @param dev      Context to initialise (output).
 * @param step     PWM channel wired to the STEP input.
 * @param dir      GPIO wired to the DIR input.
 * @param nenable  GPIO wired to the active-low nENABLE input.
 * @param ms1      GPIO wired to MS1.
 * @param ms2      GPIO wired to MS2.
 * @param ms3      GPIO wired to MS3 (may be NULL if hardwired on the board).
 * @return         ALP_OK on success, ALP_ERR_INVAL on NULL @p dev or a required
 *                 handle.
 */
alp_status_t a4988_init(a4988_t    *dev,
                        alp_pwm_t  *step,
                        alp_gpio_t *dir,
                        alp_gpio_t *nenable,
                        alp_gpio_t *ms1,
                        alp_gpio_t *ms2,
                        alp_gpio_t *ms3);

/**
 * @brief Apply a microstep resolution by latching the MS1/MS2/MS3 GPIOs.
 *
 * @param dev   Initialised context.
 * @param mode  Microstep resolution (also the MS3:MS2:MS1 pin pattern).
 * @return      ALP_OK on success, ALP_ERR_NOT_READY if not initialised,
 *              ALP_ERR_INVAL on NULL @p dev.
 */
alp_status_t a4988_set_microstep(a4988_t *dev, a4988_ustep_t mode);

/**
 * @brief Set rotation direction.
 *
 * @param dev      Initialised context.
 * @param forward  True drives DIR high, false drives it low.  The physical
 *                 sense of "forward" depends on motor wiring.
 * @return         ALP_OK on success, ALP_ERR_NOT_READY if not initialised,
 *                 ALP_ERR_INVAL on NULL @p dev.
 */
alp_status_t a4988_set_direction(a4988_t *dev, bool forward);

/**
 * @brief Set the STEP pulse rate (and thus motor speed).
 *
 * Each STEP pulse advances the rotor by one (micro)step at the resolution set
 * via a4988_set_microstep().
 *
 * @param dev      Initialised context.
 * @param freq_hz  Step frequency in Hz; 0 stops the pulse train (motor holds
 *                 position if still enabled).
 * @return         ALP_OK on success, ALP_ERR_NOT_READY if not initialised,
 *                 ALP_ERR_INVAL on NULL @p dev.
 */
alp_status_t a4988_set_step_rate(a4988_t *dev, uint32_t freq_hz);

/**
 * @brief Enable or disable the driver outputs via nENABLE.
 *
 * @param dev      Initialised context.
 * @param enabled  True drives nENABLE low (coils energised); false drives it
 *                 high (outputs tristated, motor free to spin).
 * @return         ALP_OK on success, ALP_ERR_NOT_READY if not initialised,
 *                 ALP_ERR_INVAL on NULL @p dev.
 */
alp_status_t a4988_set_enable(a4988_t *dev, bool enabled);

/**
 * @brief Release the driver context.  Idempotent; NULL tolerated.
 *
 * Does not close the borrowed PWM/GPIO handles -- the caller owns those.
 *
 * @param dev  Context to release (may be NULL).
 */
void a4988_deinit(a4988_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_A4988_H */
