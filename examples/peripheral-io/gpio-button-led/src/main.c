/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gpio-button-led -- read a button and toggle an LED, both as plain
 * GPIO, through the <alp/blocks/button_led.h> helper.
 *
 * This demo opens its pins by their BOARD-MACRO names rather than raw
 * E1M indices: `EVK_PIN_ENCODER_SW` (the user button) and
 * `EVK_PIN_LED_RED` (the status LED) come from the board's generated
 * routes header, <alp/boards/alp_e1m_evk_routes.h>.  The loader emits
 * one `#define EVK_<NAME> E1M_<...>` per `e1m_routes:` entry, so the
 * macro is the readable, app-facing pin name -- copy this example to
 * your own board, rebind those names in your board.yaml `pins:` block,
 * and the code below is unchanged.
 *
 * The macros resolve to E1M instance IDs under the hood:
 *   EVK_PIN_ENCODER_SW = E1M_GPIO_IO4   -- the encoder push switch.
 *   EVK_PIN_LED_RED    = E1M_GPIO_PWM3  -- the RGB-red PWM pad claimed
 *                                          as a digital GPIO (the
 *                                          e1m-spec "GPIO secondary";
 *                                          the EVK has no plain GPIO
 *                                          LED, so the LED rides a PWM
 *                                          pad driven as GPIO).
 *
 * native_sim wires a GPIO-emul controller in boards/, so the open /
 * configure / read / write path runs and the harness latches `done`.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/blocks/button_led.h"
#include "alp/boards/alp_e1m_evk_routes.h"

int main(void)
{
    printf("[gpio] init button=EVK_PIN_ENCODER_SW, led=EVK_PIN_LED_RED\n");

    alp_button_led_t bl;
    alp_status_t     s = alp_button_led_init(&bl, &(alp_button_led_config_t){
                                                      .button_pin_id     = EVK_PIN_ENCODER_SW,
                                                      .led_pin_id        = EVK_PIN_LED_RED,
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
