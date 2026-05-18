/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Generic button + LED block helper.  Wraps <alp/peripheral.h>
 * GPIO calls; portable across all three OS targets.
 */

#include <stddef.h>

#include "alp/blocks/button_led.h"

static void on_gpio_irq(alp_gpio_t *pin, void *user) {
    (void)pin;
    alp_button_led_t *bl = (alp_button_led_t *)user;
    if (bl == NULL) return;
    if (bl->press_cb != NULL) bl->press_cb(bl->cb_user);
}

alp_status_t alp_button_led_init(alp_button_led_t *bl,
                                 const alp_button_led_config_t *cfg) {
    if (bl == NULL || cfg == NULL) return ALP_ERR_INVAL;

    bl->button       = NULL;
    bl->led          = NULL;
    bl->press_cb     = NULL;
    bl->cb_user      = NULL;
    bl->active_low_button = cfg->active_low_button;
    bl->initialised  = false;

    bl->button = alp_gpio_open(cfg->button_pin_id);
    if (bl->button == NULL) return ALP_ERR_INVAL;

    bl->led = alp_gpio_open(cfg->led_pin_id);
    if (bl->led == NULL) {
        alp_gpio_close(bl->button);
        bl->button = NULL;
        return ALP_ERR_INVAL;
    }

    alp_status_t s = alp_gpio_configure(bl->button, ALP_GPIO_INPUT,
                                        cfg->active_low_button
                                            ? ALP_GPIO_PULL_UP
                                            : ALP_GPIO_PULL_DOWN);
    if (s != ALP_OK) goto fail;

    s = alp_gpio_configure(bl->led, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
    if (s != ALP_OK) goto fail;

    s = alp_gpio_write(bl->led, false);
    if (s != ALP_OK) goto fail;

    bl->initialised = true;
    return ALP_OK;

fail:
    alp_gpio_close(bl->led);
    alp_gpio_close(bl->button);
    bl->led = NULL;
    bl->button = NULL;
    return s;
}

alp_status_t alp_button_led_is_pressed(alp_button_led_t *bl, bool *pressed) {
    if (bl == NULL || !bl->initialised) return ALP_ERR_NOT_READY;
    if (pressed == NULL) return ALP_ERR_INVAL;
    bool level = false;
    alp_status_t s = alp_gpio_read(bl->button, &level);
    if (s != ALP_OK) return s;
    *pressed = bl->active_low_button ? !level : level;
    return ALP_OK;
}

alp_status_t alp_button_led_set(alp_button_led_t *bl, bool on) {
    if (bl == NULL || !bl->initialised) return ALP_ERR_NOT_READY;
    return alp_gpio_write(bl->led, on);
}

alp_status_t alp_button_led_toggle(alp_button_led_t *bl) {
    if (bl == NULL || !bl->initialised) return ALP_ERR_NOT_READY;
    bool level = false;
    alp_status_t s = alp_gpio_read(bl->led, &level);
    if (s != ALP_OK) return s;
    return alp_gpio_write(bl->led, !level);
}

alp_status_t alp_button_led_set_press_callback(alp_button_led_t *bl,
                                               alp_button_led_press_cb_t cb,
                                               void *user) {
    if (bl == NULL || !bl->initialised) return ALP_ERR_NOT_READY;

    if (cb == NULL) {
        bl->press_cb = NULL;
        bl->cb_user = NULL;
        return alp_gpio_irq_disable(bl->button);
    }

    bl->press_cb = cb;
    bl->cb_user = user;

    /* For active-low buttons the press is the falling edge;
     * for active-high buttons the press is the rising edge. */
    const alp_gpio_edge_t edge = bl->active_low_button
        ? ALP_GPIO_EDGE_FALLING
        : ALP_GPIO_EDGE_RISING;

    return alp_gpio_irq_enable(bl->button, edge, on_gpio_irq, bl);
}

void alp_button_led_deinit(alp_button_led_t *bl) {
    if (bl == NULL || !bl->initialised) return;
    if (bl->press_cb != NULL) {
        (void)alp_gpio_irq_disable(bl->button);
    }
    alp_gpio_close(bl->led);
    alp_gpio_close(bl->button);
    bl->button = NULL;
    bl->led = NULL;
    bl->press_cb = NULL;
    bl->cb_user = NULL;
    bl->initialised = false;
}
