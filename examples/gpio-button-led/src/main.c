/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * gpio-button-led — read a button and toggle an LED.  Uses the
 * <alp/chips/button_led.h> helper which wraps the underlying
 * <alp/peripheral.h> GPIO calls.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/chips/button_led.h"
#include "alp/e1m_pinout.h"

int main(void) {
    printf("[gpio] init button=E1M_GPIO_IO0, led=E1M_GPIO_IO1\n");

    alp_button_led_t bl;
    alp_status_t s = alp_button_led_init(&bl, &(alp_button_led_config_t){
        .button_pin_id     = E1M_GPIO_IO0,
        .led_pin_id        = E1M_GPIO_IO1,
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
        s = alp_button_led_set(&bl, on);
        printf("[gpio] led=%d status=%d\n", (int)on, (int)s);
        k_msleep(50);
    }

    bool pressed = false;
    s = alp_button_led_is_pressed(&bl, &pressed);
    printf("[gpio] is_pressed -> status=%d pressed=%d\n",
           (int)s, (int)pressed);

    alp_button_led_deinit(&bl);
    printf("[gpio] done\n");
    return 0;
}
