/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gpio-button-led -- read a button and toggle an LED, both as plain
 * GPIO, through the <alp/blocks/button_led.h> helper.
 *
 * The interesting part is the LED.  E1M makes GPIO a *universal
 * secondary* on every digital pad (e1m-spec STANDARD.md "GPIO
 * secondary"), so a pad whose default function is PWM/ADC/etc. can be
 * claimed as a digital GPIO instead.  The E1M-EVK has no dedicated
 * GPIO LED -- its only user LEDs are the RGB cluster on the PWM pads --
 * so this demo drives the red LED's pad (PWM3) as a GPIO via the
 * parallel `E1M_GPIO_PWM3` index.  The button is the encoder push
 * switch on `E1M_GPIO_IO4` (active-low).
 *
 * The portable rule (e1m_pinout.h "Pin-as-GPIO fallback"): open a
 * pad's analog/timer function with its peripheral id (`E1M_PWM3` ->
 * alp_pwm_open), OR claim it as a digital GPIO with its
 * `E1M_GPIO_<class><N>` index (`E1M_GPIO_PWM3` -> alp_gpio_open).
 * Don't hold both handles against the same pad -- the silicon is
 * shared.
 *
 * native_sim has a GPIO-emul controller wired in boards/, so the
 * open / configure / read / write path runs and the harness latches
 * `done`:
 *
 *   [gpio] init button=E1M_GPIO_IO4, led=E1M_GPIO_PWM3 (PWM3 pad as GPIO)
 *   [gpio] led=0 status=0
 *   ...
 *   [gpio] is_pressed -> status=0 pressed=0
 *   [gpio] done
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/blocks/button_led.h"
#include "alp/e1m_pinout.h"

int main(void)
{
    printf("[gpio] init button=E1M_GPIO_IO4, led=E1M_GPIO_PWM3 "
           "(PWM3 pad as GPIO)\n");

    alp_button_led_t bl;
    alp_status_t     s =
        alp_button_led_init(&bl, &(alp_button_led_config_t){
                                     .button_pin_id = E1M_GPIO_IO4, /* encoder push switch */
                                     .led_pin_id = E1M_GPIO_PWM3, /* RGB-red pad, claimed as GPIO */
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
