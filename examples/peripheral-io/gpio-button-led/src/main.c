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
 *     BOARD_PIN_ENCODER_SW = EVK_PIN_ENCODER_SW  = E1M_GPIO_IO4
 *     BOARD_PIN_LED_RED    = EVK_PIN_LED_RED      = E1M_GPIO_PWM3
 *       (the RGB-red PWM pad claimed as a digital GPIO; the E1M EVK
 *       has no plain GPIO LED, so the LED rides a PWM pad as GPIO)
 *   E1M-X EVK:
 *     BOARD_PIN_ENCODER_SW = XEVK_PIN_ENCODER_SW = E1M_X_GPIO_IO28
 *     BOARD_PIN_LED_RED    = XEVK_PIN_LED_RED     = E1M_X_GPIO_PWM5
 *       (similarly the X EVK's RGB-red PWM pad driven as GPIO)
 *
 * native_sim wires a GPIO-emul controller in boards/, so the open /
 * configure / read / write path runs and the harness latches `done`.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/blocks/button_led.h"
#include "alp/board.h"

int main(void)
{
    printf("[gpio] init button=BOARD_PIN_ENCODER_SW, led=BOARD_PIN_LED_RED\n");

    alp_button_led_t bl;
    alp_status_t     s = alp_button_led_init(&bl, &(alp_button_led_config_t){
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
        k_msleep(50);
    }

    bool pressed = false;
    s            = alp_button_led_is_pressed(&bl, &pressed);
    printf("[gpio] is_pressed -> status=%d pressed=%d\n", (int)s, (int)pressed);

    alp_button_led_deinit(&bl);
    printf("[gpio] done\n");
    return 0;
}
