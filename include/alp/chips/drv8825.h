/*
 * Copyright 2026 ALP Lab AB
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

/** Microstep mode selection (M2:M1:M0). */
typedef enum {
    DRV8825_USTEP_FULL  = 0, /**< 000 */
    DRV8825_USTEP_HALF  = 1,
    DRV8825_USTEP_1_4   = 2,
    DRV8825_USTEP_1_8   = 3,
    DRV8825_USTEP_1_16  = 4,
    DRV8825_USTEP_1_32  = 5, /**< 5..7 all decode to 1/32. */
} drv8825_ustep_t;

typedef struct {
    alp_pwm_t  *step;
    alp_gpio_t *dir;
    alp_gpio_t *nenbl; /**< Active-low enable.  Optional. */
    alp_gpio_t *m0;
    alp_gpio_t *m1;
    alp_gpio_t *m2;
    bool        initialised;
} drv8825_t;

/** @brief Bind context to caller-opened PWM + GPIO handles. */
alp_status_t drv8825_init(drv8825_t  *dev,
                          alp_pwm_t  *step,
                          alp_gpio_t *dir,
                          alp_gpio_t *nenbl,
                          alp_gpio_t *m0,
                          alp_gpio_t *m1,
                          alp_gpio_t *m2);

/** @brief Apply a microstep mode by latching M0/M1/M2 GPIOs. */
alp_status_t drv8825_set_microstep(drv8825_t *dev, drv8825_ustep_t mode);

/** @brief Set rotation direction (true = forward / DIR high). */
alp_status_t drv8825_set_direction(drv8825_t *dev, bool forward);

/**
 * @brief Set step-pulse rate by configuring the STEP PWM frequency.
 *
 * @param dev      Initialised context.
 * @param freq_hz  Step frequency in Hz (0 = stop; capped at 250 kHz
 *                 per datasheet).
 */
alp_status_t drv8825_set_step_rate(drv8825_t *dev, uint32_t freq_hz);

/** @brief Enable (false) or disable (true) the driver output. */
alp_status_t drv8825_set_sleep(drv8825_t *dev, bool sleeping);

/** @brief Release driver context. */
void drv8825_deinit(drv8825_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_DRV8825_H */
