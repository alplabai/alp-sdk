/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file button_led.h
 * @brief Generic button + LED block helper.
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *
 * Consumed by alp-studio block `blk_button_led`.  Unlike
 * `lsm6dso_*` and `ssd1306_*`, **this helper carries the `alp_`
 * prefix** because it's an SDK-level block utility, not a binding
 * to a single third-party IC.  The button could be any momentary
 * switch and the LED any indicator — the helper just orchestrates
 * the GPIO pair.
 *
 * Wraps `<alp/peripheral.h>` GPIO calls; portable across all three
 * OS targets.
 */

#ifndef ALP_CHIPS_BUTTON_LED_H
#define ALP_CHIPS_BUTTON_LED_H

#include <stdbool.h>
#include <stdint.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Callback fired by `alp_button_led_set_press_callback` on each press. */
typedef void (*alp_button_led_press_cb_t)(void *user);

/** Driver context.  Treat as opaque. */
typedef struct {
    alp_gpio_t                *button;
    alp_gpio_t                *led;
    alp_button_led_press_cb_t  press_cb;
    void                      *cb_user;
    bool                       active_low_button;  /**< Most dev boards pull buttons up; press = 0. */
    bool                       initialised;
} alp_button_led_t;

typedef struct {
    uint32_t button_pin_id;     /**< `pin_id` in alp,pin-array. */
    uint32_t led_pin_id;
    bool     active_low_button; /**< true if the button reads 0 when pressed. */
} alp_button_led_config_t;

/**
 * @brief Initialise the helper from a button+LED pin pair.
 *
 * Opens both GPIOs via `alp_gpio_open`, configures the button as
 * input (pull-up if `active_low_button`) and the LED as output
 * (initially off).
 */
alp_status_t alp_button_led_init(alp_button_led_t *bl,
                                 const alp_button_led_config_t *cfg);

/** True if the button is currently in the pressed state. */
alp_status_t alp_button_led_is_pressed(alp_button_led_t *bl, bool *pressed);

/** Drive the LED on or off. */
alp_status_t alp_button_led_set(alp_button_led_t *bl, bool on);

/** Toggle the LED. */
alp_status_t alp_button_led_toggle(alp_button_led_t *bl);

/**
 * @brief Register a callback fired on every detected press.
 *
 * Internally enables a GPIO interrupt with the appropriate edge for
 * the configured polarity.  Pass `cb = NULL` to clear.
 */
alp_status_t alp_button_led_set_press_callback(alp_button_led_t *bl,
                                               alp_button_led_press_cb_t cb,
                                               void *user);

/** Release the helper.  Closes the underlying GPIOs. */
void         alp_button_led_deinit(alp_button_led_t *bl);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_CHIPS_BUTTON_LED_H */
