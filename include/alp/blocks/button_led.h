/*
 * Copyright 2026 Alp Lab AB
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
 * Consumed by alp-studio block `blk_button_led`.  Lives under
 * `<alp/blocks/>` (NOT `<alp/chips/>`) because this is an
 * SDK-level *block* utility, not a binding to a single third-party
 * IC.  Hence the `alp_` prefix on every symbol — the button could
 * be any momentary switch and the LED any indicator; the helper
 * just orchestrates the GPIO pair.  See `blocks/README.md` for the
 * full block-vs-chip rationale.
 *
 * Wraps `<alp/peripheral.h>` GPIO calls; portable across all three
 * OS targets.
 */

#ifndef ALP_BLOCKS_BUTTON_LED_H
#define ALP_BLOCKS_BUTTON_LED_H

#include <stdbool.h>
#include <stdint.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Callback fired by `alp_button_led_set_press_callback` on each press. */
typedef void (*alp_button_led_press_cb_t)(void *user);

/** @brief Driver context.  Treat as opaque; populated by @ref alp_button_led_init. */
typedef struct {
	alp_gpio_t               *button;            /**< Button GPIO handle (input). */
	alp_gpio_t               *led;               /**< LED GPIO handle (output). */
	alp_button_led_press_cb_t press_cb;          /**< Press callback, or NULL if unset. */
	void                     *cb_user;           /**< Opaque pointer forwarded to @c press_cb. */
	bool                      active_low_button; /**< Most dev boards pull buttons up; press = 0. */
	bool                      initialised; /**< true once @ref alp_button_led_init succeeded. */
} alp_button_led_t;

/** @brief Configuration passed to @ref alp_button_led_init. */
typedef struct {
	uint32_t button_pin_id;     /**< `pin_id` in alp,pin-array for the button. */
	uint32_t led_pin_id;        /**< `pin_id` in alp,pin-array for the LED. */
	bool     active_low_button; /**< true if the button reads 0 when pressed. */
} alp_button_led_config_t;

/**
 * @brief Initialise the helper from a button+LED pin pair.
 *
 * Opens both GPIOs via `alp_gpio_open`, configures the button as
 * input (pull-up if `active_low_button`) and the LED as output
 * (initially off).
 *
 * @param[out] bl   Caller-allocated context to initialise.
 * @param[in]  cfg  Pin pair + polarity.  Must be non-NULL.
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_IO (GPIO open/config failed).
 */
alp_status_t alp_button_led_init(alp_button_led_t *bl, const alp_button_led_config_t *cfg);

/**
 * @brief Sample the button's current debounced level.
 *
 * @param[in]  bl       Context from @ref alp_button_led_init.
 * @param[out] pressed  Receives true when the button is held down,
 *                      accounting for @c active_low_button polarity.
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_IO.
 */
alp_status_t alp_button_led_is_pressed(alp_button_led_t *bl, bool *pressed);

/**
 * @brief Drive the LED on or off.
 *
 * @param[in] bl  Context from @ref alp_button_led_init.
 * @param[in] on  true = LED on, false = LED off.
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_IO.
 */
alp_status_t alp_button_led_set(alp_button_led_t *bl, bool on);

/**
 * @brief Toggle the LED's current state.
 *
 * @param[in] bl  Context from @ref alp_button_led_init.
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_IO.
 */
alp_status_t alp_button_led_toggle(alp_button_led_t *bl);

/**
 * @brief Register a callback fired on every detected press.
 *
 * Internally enables a GPIO interrupt with the appropriate edge for
 * the configured polarity.  Pass `cb = NULL` to clear.
 *
 * @param[in] bl    Context from @ref alp_button_led_init.
 * @param[in] cb    Callback fired on each press, or NULL to disable.
 *                  Runs in interrupt context on the Zephyr/baremetal backends.
 * @param[in] user  Opaque pointer forwarded to @p cb.
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_IO.
 */
alp_status_t
alp_button_led_set_press_callback(alp_button_led_t *bl, alp_button_led_press_cb_t cb, void *user);

/**
 * @brief Release the helper, closing the underlying GPIOs.  NULL is a no-op.
 *
 * @param[in] bl  Context from @ref alp_button_led_init, or NULL.
 */
void alp_button_led_deinit(alp_button_led_t *bl);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_BLOCKS_BUTTON_LED_H */
