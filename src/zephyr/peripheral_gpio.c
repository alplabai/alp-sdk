/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/peripheral.h> — GPIO.
 *
 * Pin lookup is via the `alp,pin-array` devicetree node — a single
 * node carrying a `gpios` property with one phandle/spec per pin id.
 * See dts/bindings/alp,pin-array.yaml.
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/sys/util.h>

#include "alp/peripheral.h"
#include "handles.h"

#define DT_DRV_COMPAT alp_pin_array

#define ALP_PIN_NODE  DT_INST(0, alp_pin_array)

#if DT_NODE_EXISTS(ALP_PIN_NODE)
#  define ALP_PIN_AVAILABLE 1
#  define ALP_PIN_COUNT     DT_PROP_LEN(ALP_PIN_NODE, gpios)
#else
#  define ALP_PIN_AVAILABLE 0
#  define ALP_PIN_COUNT     0
#endif

#if ALP_PIN_AVAILABLE
#  define ALP_PIN_BY_IDX(idx) GPIO_DT_SPEC_GET_BY_IDX(ALP_PIN_NODE, gpios, idx)

#  define ALP_PIN_ENTRY(i, _) ALP_PIN_BY_IDX(i),

static const struct gpio_dt_spec alp_pins[] = {
    LISTIFY(ALP_PIN_COUNT, ALP_PIN_ENTRY, ())
};
#endif

bool alp_z_gpio_resolve(uint32_t pin_id, struct gpio_dt_spec *out) {
#if ALP_PIN_AVAILABLE
    if (pin_id >= ARRAY_SIZE(alp_pins)) return false;
    *out = alp_pins[pin_id];
    return true;
#else
    (void)pin_id; (void)out;
    return false;
#endif
}

static gpio_flags_t to_gpio_flags(alp_gpio_dir_t dir, alp_gpio_pull_t pull) {
    gpio_flags_t f = (dir == ALP_GPIO_OUTPUT) ? GPIO_OUTPUT : GPIO_INPUT;
    if (pull == ALP_GPIO_PULL_UP)   f |= GPIO_PULL_UP;
    if (pull == ALP_GPIO_PULL_DOWN) f |= GPIO_PULL_DOWN;
    return f;
}

static gpio_flags_t to_gpio_irq_flags(alp_gpio_edge_t edge) {
    switch (edge) {
    case ALP_GPIO_EDGE_RISING:  return GPIO_INT_EDGE_RISING;
    case ALP_GPIO_EDGE_FALLING: return GPIO_INT_EDGE_FALLING;
    case ALP_GPIO_EDGE_BOTH:    return GPIO_INT_EDGE_BOTH;
    default:                    return GPIO_INT_DISABLE;
    }
}

static alp_status_t errno_to_alp(int err) {
    switch (err) {
    case 0:        return ALP_OK;
    case -EINVAL:  return ALP_ERR_INVAL;
    case -EBUSY:   return ALP_ERR_BUSY;
    case -EIO:     return ALP_ERR_IO;
    case -ENOTSUP:
    case -ENOSYS:  return ALP_ERR_NOSUPPORT;
    default:       return ALP_ERR_IO;
    }
}

alp_gpio_t *alp_gpio_open(uint32_t pin_id) {
    struct gpio_dt_spec spec;
    if (!alp_z_gpio_resolve(pin_id, &spec)) return NULL;
    if (!device_is_ready(spec.port)) return NULL;

    struct alp_gpio *h = alp_z_gpio_pool_acquire();
    if (h == NULL) return NULL;
    h->pin_id = pin_id;
    h->spec   = spec;
    h->dir    = ALP_GPIO_INPUT;
    h->pull   = ALP_GPIO_PULL_NONE;
    h->edge   = ALP_GPIO_EDGE_NONE;
    return h;
}

void alp_gpio_close(alp_gpio_t *pin) {
    if (pin == NULL || !pin->in_use) return;
    if (pin->edge != ALP_GPIO_EDGE_NONE) {
        gpio_pin_interrupt_configure_dt(&pin->spec, GPIO_INT_DISABLE);
        gpio_remove_callback(pin->spec.port, &pin->zcb);
    }
    alp_z_gpio_pool_release(pin);
}

alp_status_t alp_gpio_configure(alp_gpio_t *pin,
                                alp_gpio_dir_t dir,
                                alp_gpio_pull_t pull) {
    if (pin == NULL || !pin->in_use) return ALP_ERR_NOT_READY;
    int err = gpio_pin_configure_dt(&pin->spec, to_gpio_flags(dir, pull));
    if (err == 0) {
        pin->dir = dir;
        pin->pull = pull;
    }
    return errno_to_alp(err);
}

alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level) {
    if (pin == NULL || !pin->in_use) return ALP_ERR_NOT_READY;
    return errno_to_alp(gpio_pin_set_dt(&pin->spec, level ? 1 : 0));
}

alp_status_t alp_gpio_read(alp_gpio_t *pin, bool *level) {
    if (pin == NULL || !pin->in_use) return ALP_ERR_NOT_READY;
    if (level == NULL) return ALP_ERR_INVAL;
    int v = gpio_pin_get_dt(&pin->spec);
    if (v < 0) return errno_to_alp(v);
    *level = (v != 0);
    return ALP_OK;
}

static void alp_gpio_isr_thunk(const struct device *port,
                               struct gpio_callback *cb,
                               gpio_port_pins_t pins) {
    ARG_UNUSED(port); ARG_UNUSED(pins);
    struct alp_gpio *h = CONTAINER_OF(cb, struct alp_gpio, zcb);
    if (h->cb != NULL) {
        h->cb(h, h->cb_user);
    }
}

alp_status_t alp_gpio_irq_enable(alp_gpio_t *pin,
                                 alp_gpio_edge_t edge,
                                 alp_gpio_cb_t cb,
                                 void *user) {
    if (pin == NULL || !pin->in_use) return ALP_ERR_NOT_READY;
    if (edge == ALP_GPIO_EDGE_NONE || cb == NULL) return ALP_ERR_INVAL;

    pin->cb      = cb;
    pin->cb_user = user;
    pin->edge    = edge;

    gpio_init_callback(&pin->zcb, alp_gpio_isr_thunk, BIT(pin->spec.pin));
    int err = gpio_add_callback(pin->spec.port, &pin->zcb);
    if (err != 0) return errno_to_alp(err);

    err = gpio_pin_interrupt_configure_dt(&pin->spec, to_gpio_irq_flags(edge));
    if (err != 0) {
        gpio_remove_callback(pin->spec.port, &pin->zcb);
        pin->edge = ALP_GPIO_EDGE_NONE;
    }
    return errno_to_alp(err);
}

alp_status_t alp_gpio_irq_disable(alp_gpio_t *pin) {
    if (pin == NULL || !pin->in_use) return ALP_ERR_NOT_READY;
    if (pin->edge == ALP_GPIO_EDGE_NONE) return ALP_OK;
    int err = gpio_pin_interrupt_configure_dt(&pin->spec, GPIO_INT_DISABLE);
    gpio_remove_callback(pin->spec.port, &pin->zcb);
    pin->edge = ALP_GPIO_EDGE_NONE;
    return errno_to_alp(err);
}
