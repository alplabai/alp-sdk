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

/** Microstep mode selection (MS3:MS2:MS1). */
typedef enum {
    A4988_USTEP_FULL = 0, /**< 000 */
    A4988_USTEP_HALF = 1,
    A4988_USTEP_1_4  = 2,
    A4988_USTEP_1_8  = 3,
    A4988_USTEP_1_16 = 7, /**< 111 */
} a4988_ustep_t;

typedef struct {
    alp_pwm_t  *step;
    alp_gpio_t *dir;
    alp_gpio_t *nenable;
    alp_gpio_t *ms1;
    alp_gpio_t *ms2;
    alp_gpio_t *ms3;
    bool        initialised;
} a4988_t;

/** @brief Bind context to caller-opened PWM + GPIO handles. */
alp_status_t a4988_init(a4988_t    *dev,
                        alp_pwm_t  *step,
                        alp_gpio_t *dir,
                        alp_gpio_t *nenable,
                        alp_gpio_t *ms1,
                        alp_gpio_t *ms2,
                        alp_gpio_t *ms3);

/** @brief Apply a microstep mode by latching MS1/2/3 GPIOs. */
alp_status_t a4988_set_microstep(a4988_t *dev, a4988_ustep_t mode);

/** @brief Set rotation direction (true = DIR high). */
alp_status_t a4988_set_direction(a4988_t *dev, bool forward);

/** @brief Set step pulse rate.  freq_hz = 0 stops. */
alp_status_t a4988_set_step_rate(a4988_t *dev, uint32_t freq_hz);

/** @brief Enable / disable the driver output. */
alp_status_t a4988_set_enable(a4988_t *dev, bool enabled);

/** @brief Release driver context. */
void a4988_deinit(a4988_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_A4988_H */
