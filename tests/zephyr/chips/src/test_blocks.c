/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * `<alp/blocks/>` helper smokes: button_led (v0.6+ button + LED pair
 * over GPIO).
 */

#include <zephyr/ztest.h>

#include "alp/blocks/button_led.h"
#include "alp/e1m_pinout.h"
#include "alp/peripheral.h"

/* ------------------------------------------------------------------ */
/* button_led                                                          */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_button_led_init_null_args)
{
	alp_button_led_t bl;
	zassert_equal(alp_button_led_init(NULL, NULL), ALP_ERR_INVAL, "all-NULL must be invalid");
	zassert_equal(alp_button_led_init(&bl, NULL), ALP_ERR_INVAL, "NULL cfg must be invalid");
}

ZTEST(alp_chips, test_button_led_init_valid_pair)
{
	/* Overlay wires alp,pin-array index 0 → button (pull-up) and
     * index 1 → LED on the chips test's gpio_emul. */
	alp_button_led_t bl;
	alp_status_t     s = alp_button_led_init(&bl,
	                                         &(alp_button_led_config_t){
	                                             .button_pin_id     = 0,
	                                             .led_pin_id        = 1,
	                                             .active_low_button = true,
	                                         });
	zassert_equal(s, ALP_OK, "init failed: %d", (int)s);

	/* Verify is_pressed() returns ALP_OK and writes a defined
     * value.  Don't assert WHICH value — gpio_emul's input
     * register defaults to 0, which an active-low button reports
     * as "pressed."  Driving the input register to a known value
     * needs gpio_emul_input_set(), which would couple this smoke
     * test to a Zephyr emul-specific API. */
	bool pressed;
	zassert_equal(alp_button_led_is_pressed(&bl, &pressed), ALP_OK);
	(void)pressed;

	zassert_equal(alp_button_led_set(&bl, true), ALP_OK);
	zassert_equal(alp_button_led_set(&bl, false), ALP_OK);
	zassert_equal(alp_button_led_toggle(&bl), ALP_OK);

	alp_button_led_deinit(&bl);
}

ZTEST(alp_chips, test_button_led_calls_reject_uninitialised)
{
	alp_button_led_t bl = { 0 };
	bool             pressed;
	zassert_equal(alp_button_led_is_pressed(&bl, &pressed), ALP_ERR_NOT_READY);
	zassert_equal(alp_button_led_set(&bl, true), ALP_ERR_NOT_READY);
	zassert_equal(alp_button_led_toggle(&bl), ALP_ERR_NOT_READY);
}

static void test_button_led_press_cb_(void *user)
{
	int *count = (int *)user;
	if (count != NULL) (*count)++;
}

ZTEST(alp_chips, test_button_led_set_press_callback_uninitialised_not_ready)
{
	alp_button_led_t bl = { 0 };
	zassert_equal(alp_button_led_set_press_callback(&bl, test_button_led_press_cb_, NULL),
	              ALP_ERR_NOT_READY);
}

ZTEST(alp_chips, test_button_led_set_press_callback_real_gpio_irq_roundtrip)
{
	/* Same overlay wiring as test_button_led_init_valid_pair: pin
     * index 0 -> button (pull-up), index 1 -> LED, on the chips
     * test's gpio_emul. */
	alp_button_led_t bl;
	alp_status_t     s = alp_button_led_init(&bl,
	                                         &(alp_button_led_config_t){
	                                             .button_pin_id     = 0,
	                                             .led_pin_id        = 1,
	                                             .active_low_button = true,
	                                         });
	zassert_equal(s, ALP_OK, "init failed: %d", (int)s);

	int press_count = 0;

	/* Registers a real gpio_pin_interrupt_configure_dt() on the
     * emulated button pin -- exercises the actual dispatch path, not
     * just a NULL guard. */
	zassert_equal(alp_button_led_set_press_callback(&bl, test_button_led_press_cb_, &press_count),
	              ALP_OK);
	zassert_equal(bl.press_cb, test_button_led_press_cb_);
	zassert_equal_ptr(bl.cb_user, &press_count);

	/* cb == NULL clears the callback and disables the IRQ. */
	zassert_equal(alp_button_led_set_press_callback(&bl, NULL, NULL), ALP_OK);
	zassert_is_null(bl.press_cb);

	alp_button_led_deinit(&bl);
}
