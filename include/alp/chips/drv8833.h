/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file drv8833.h
 * @brief TI DRV8833 dual H-bridge brushed-DC motor driver.
 *
 * Carrier-board control surface: 2 H-bridges, four PWM-capable
 * inputs IN1/IN2/IN3/IN4 + nSLEEP enable.  No on-chip register
 * file -- the driver just orchestrates the four PWMs + the sleep
 * GPIO.  All PWM access goes through the portable `<alp/pwm.h>`
 * surface (see §SDK-rule "Portable peripheral surfaces only").
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 * @par Verification status: [UNTESTED] — driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Datasheet: TI DRV8833 SLVSAR1F (Apr 2017).
 */

#ifndef ALP_CHIPS_DRV8833_H
#define ALP_CHIPS_DRV8833_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"
#include "alp/pwm.h" /* alp_pwm_t lives here, not in peripheral.h */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    alp_pwm_t  *in1; /**< Channel-A input 1. */
    alp_pwm_t  *in2; /**< Channel-A input 2. */
    alp_pwm_t  *in3; /**< Channel-B input 1. */
    alp_pwm_t  *in4; /**< Channel-B input 2. */
    alp_gpio_t *nsleep; /**< Active-low enable; may be NULL when tied high. */
    bool        initialised;
} drv8833_t;

/**
 * @brief Bind context to caller-opened PWM + GPIO handles.
 *
 * @param dev     Output: caller-allocated context.
 * @param in1     Open PWM for A-side input 1.
 * @param in2     Open PWM for A-side input 2.
 * @param in3     Open PWM for B-side input 1.
 * @param in4     Open PWM for B-side input 2.
 * @param nsleep  Open GPIO for nSLEEP (NULL if tied permanently high).
 * @return `ALP_OK` on success.
 */
alp_status_t drv8833_init(drv8833_t  *dev,
                          alp_pwm_t  *in1,
                          alp_pwm_t  *in2,
                          alp_pwm_t  *in3,
                          alp_pwm_t  *in4,
                          alp_gpio_t *nsleep);

/**
 * @brief Set channel-A pulse-width.
 *
 * @param dev          Initialised context.
 * @param pulse_ns     Active-level pulse width in ns; must be
 *                     <= the channel's configured PWM period.
 *                     Negative drives reverse, positive drives
 *                     forward, zero coasts.
 * @return `ALP_OK` on success.
 */
alp_status_t drv8833_set_a(drv8833_t *dev, int32_t pulse_ns);

/** @brief Set channel-B pulse-width.  See @ref drv8833_set_a. */
alp_status_t drv8833_set_b(drv8833_t *dev, int32_t pulse_ns);

/** @brief Toggle the device sleep state (active-low). */
alp_status_t drv8833_set_sleep(drv8833_t *dev, bool sleeping);

/** @brief Release driver context. */
void drv8833_deinit(drv8833_t *dev);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_DRV8833_H */
