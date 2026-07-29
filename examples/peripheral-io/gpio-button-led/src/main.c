/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gpio-button-led -- read a button and toggle an LED, both as plain
 * GPIO, through the <alp/blocks/button_led.h> helper.
 *
 * This demo opens its pins by portable BOARD_* alias names from
 * <alp/board.h>: `BOARD_PIN_ENCODER_SW` (the user button) and
 * `BOARD_PIN_LED_RED` (the status LED).  The facade selects the active
 * board's generated routes header at compile time via ALP_BOARD_<SLUG>,
 * so the same source builds for both EVKs without changes.
 *
 * Per-board resolution:
 *   E1M EVK:
 *     BOARD_PIN_ENCODER_SW = EVK_PIN_ENCODER_SW  = ALP_E1M_GPIO_IO4
 *     BOARD_PIN_LED_RED    = EVK_PIN_LED_RED      = ALP_E1M_GPIO_PWM3
 *       (the RGB-red PWM pad claimed as a digital GPIO; the E1M EVK
 *       has no plain GPIO LED, so the LED rides a PWM pad as GPIO)
 *   E1M-X EVK:
 *     BOARD_PIN_ENCODER_SW = XEVK_PIN_ENCODER_SW = ALP_E1M_X_GPIO_IO28
 *     BOARD_PIN_LED_RED    = XEVK_PIN_LED_RED     = ALP_E1M_X_GPIO_PWM5
 *       (similarly the X EVK's RGB-red PWM pad driven as GPIO)
 *
 * native_sim wires a GPIO-emul controller in boards/, so the open /
 * configure / read / write path runs and the harness latches `done`.
 * On real hardware the demo keeps running after `done`: it blinks the
 * LED forever at a slow, visible 500 ms on / 500 ms off cadence so a
 * fresh-out-of-the-box board shows something happening at the bench.
 * That endless loop is skipped under native_sim (guarded on
 * CONFIG_BOARD_NATIVE_SIM in main()) so the twister harness still
 * gets a clean, prompt exit.
 */

#include <stdio.h>

#include "alp/peripheral.h"

#include "alp/blocks/button_led.h"
#include "alp/board.h"

int main(void)
{
	/* Bring up the SDK runtime before anything else -- thin today,
	 * but future backends rely on it (see <alp/peripheral.h>). */
	(void)alp_init();

	printf("[gpio] init button=BOARD_PIN_ENCODER_SW, led=BOARD_PIN_LED_RED\n");

	alp_button_led_t bl;
	alp_status_t     s = alp_button_led_init(&bl,
	                                         &(alp_button_led_config_t){
	                                             .button_pin_id     = BOARD_PIN_ENCODER_SW,
	                                             .led_pin_id        = BOARD_PIN_LED_RED,
	                                             .active_low_button = true,
	                                         });
	if (s != ALP_OK) {
		printf("[gpio] init failed: status=%d\n", (int)s);
		printf("[gpio] done\n");
		return 0;
	}

	/* Quick lifecycle exercise: toggle 4 times and read the button
     * state once. */
	for (int i = 0; i < 4; i++) {
		bool on = (i & 1);
		s       = alp_button_led_set(&bl, on);
		printf("[gpio] led=%d status=%d\n", (int)on, (int)s);
		alp_delay_ms(50);
	}

	bool pressed = false;
	s            = alp_button_led_is_pressed(&bl, &pressed);
	printf("[gpio] is_pressed -> status=%d pressed=%d\n", (int)s, (int)pressed);

	printf("[gpio] done\n");

#ifdef CONFIG_BOARD_NATIVE_SIM
	/* native_sim: the twister harness only waits for the "done" line
	 * above and then tears the process down, so exit cleanly instead
	 * of falling into the endless blink below. */
	alp_button_led_deinit(&bl);
	return 0;
#else
	/* Real hardware: this is the "fresh customer sees a blinking LED"
     * acceptance target, so keep driving the pin forever after
     * "done".  Deliberately no deinit here -- deinit would leave the
     * LED off, and every iteration below still needs the handle.  Do
     * NOT delete this #ifdef to "simplify" an unconditional loop:
     * that would also hang under native_sim and break both twister
     * tests in testcase.yaml. */
	for (;;) {
		alp_button_led_set(&bl, true);
		alp_delay_ms(500);
		alp_button_led_set(&bl, false);
		alp_delay_ms(500);
	}
#endif
}
