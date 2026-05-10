/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bare-metal Alif Ensemble GPIO wrapper for <alp/peripheral.h>.
 * See vendors/alif/i2c.c for the gating + scaffolding rationale.
 *
 * Alif's GPIO controllers expose 8 ports of 8 pins each via the
 * Driver_GPIO0..7 CMSIS instances; the wrapper packs (port, pin)
 * into the studio-supplied pin_id as ((port << 3) | pin) so the
 * 0..63 contiguous space matches <alp/e1m_pinout.h>'s GPIO_IO0..25
 * + the per-class pads (PWM0..7, ENC0..3, etc.).
 */

#include "alp/peripheral.h"

#if defined(ALP_HAS_ALIF_HAL)

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "Driver_GPIO.h"

extern ARM_DRIVER_GPIO        Driver_GPIO0;
extern ARM_DRIVER_GPIO        Driver_GPIO1;
extern ARM_DRIVER_GPIO        Driver_GPIO2;
extern ARM_DRIVER_GPIO        Driver_GPIO3;
extern ARM_DRIVER_GPIO        Driver_GPIO4;
extern ARM_DRIVER_GPIO        Driver_GPIO5;
extern ARM_DRIVER_GPIO        Driver_GPIO6;
extern ARM_DRIVER_GPIO        Driver_GPIO7;

static ARM_DRIVER_GPIO *const alp_alif_gpio_drivers[] = {
    &Driver_GPIO0, &Driver_GPIO1, &Driver_GPIO2, &Driver_GPIO3,
    &Driver_GPIO4, &Driver_GPIO5, &Driver_GPIO6, &Driver_GPIO7,
};

struct alp_gpio_handle {
    ARM_DRIVER_GPIO *drv;
    uint8_t          pin;
    bool             in_use;
};

#define ALP_ALIF_MAX_GPIO_HANDLES 32
static struct alp_gpio_handle  alp_alif_gpio_pool[ALP_ALIF_MAX_GPIO_HANDLES];

static struct alp_gpio_handle *gpio_pool_acquire(void)
{
    for (size_t i = 0; i < ALP_ALIF_MAX_GPIO_HANDLES; ++i) {
        if (!alp_alif_gpio_pool[i].in_use) {
            alp_alif_gpio_pool[i].in_use = true;
            return &alp_alif_gpio_pool[i];
        }
    }
    return NULL;
}

alp_gpio_t *alp_gpio_open(uint32_t pin_id)
{
    uint32_t port_idx = pin_id >> 3;
    uint8_t  pin_idx  = (uint8_t)(pin_id & 0x7);
    if (port_idx >= sizeof(alp_alif_gpio_drivers) / sizeof(alp_alif_gpio_drivers[0])) {
        return NULL;
    }
    ARM_DRIVER_GPIO *d = alp_alif_gpio_drivers[port_idx];
    if (d == NULL) return NULL;
    if (d->Initialize(pin_idx, NULL) != ARM_DRIVER_OK) return NULL;
    if (d->PowerControl(pin_idx, ARM_POWER_FULL) != ARM_DRIVER_OK) return NULL;

    struct alp_gpio_handle *h = gpio_pool_acquire();
    if (h == NULL) return NULL;
    h->drv = d;
    h->pin = pin_idx;
    return (alp_gpio_t *)h;
}

alp_status_t alp_gpio_configure(alp_gpio_t *pin, alp_gpio_dir_t dir, alp_gpio_pull_t pull)
{
    if (pin == NULL) return ALP_ERR_NOT_READY;
    struct alp_gpio_handle *h = (struct alp_gpio_handle *)pin;
    /* Direction: Alif's GPIO control codes 0=input, 1=output. */
    int32_t dr = h->drv->SetDirection(h->pin, (dir == ALP_GPIO_OUTPUT) ? 1 : 0);
    if (dr != ARM_DRIVER_OK) return ALP_ERR_IO;
    /* Pull-up / pull-down via Control() with vendor-specific opcode.
     * Encoded as 0x10 | pull-mode in Alif's HAL.  Pass-through for v0.2. */
    (void)pull;
    return ALP_OK;
}

alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level)
{
    if (pin == NULL) return ALP_ERR_NOT_READY;
    struct alp_gpio_handle *h = (struct alp_gpio_handle *)pin;
    return (h->drv->SetValue(h->pin, level ? 1 : 0) == ARM_DRIVER_OK) ? ALP_OK : ALP_ERR_IO;
}

alp_status_t alp_gpio_read(alp_gpio_t *pin, bool *level)
{
    if (pin == NULL || level == NULL) return ALP_ERR_INVAL;
    struct alp_gpio_handle *h = (struct alp_gpio_handle *)pin;
    uint32_t                v = 0;
    if (h->drv->GetValue(h->pin, &v) != ARM_DRIVER_OK) return ALP_ERR_IO;
    *level = (v != 0);
    return ALP_OK;
}

alp_status_t alp_gpio_irq_enable(alp_gpio_t *pin, alp_gpio_edge_t edge, alp_gpio_cb_t cb,
                                 void *user)
{
    (void)pin;
    (void)edge;
    (void)cb;
    (void)user;
    /* Alif's CMSIS-Driver GPIO doesn't expose IRQ registration through
     * the standard ARM_DRIVER_GPIO surface in the public pack -- the
     * interrupt path lands in v0.2.x once we wire it through the
     * vendor's `gpio_irq_register` helper directly. */
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_gpio_irq_disable(alp_gpio_t *pin)
{
    (void)pin;
    return ALP_ERR_NOSUPPORT;
}

void alp_gpio_close(alp_gpio_t *pin)
{
    if (pin == NULL) return;
    struct alp_gpio_handle *h = (struct alp_gpio_handle *)pin;
    (void)h->drv->PowerControl(h->pin, ARM_POWER_OFF);
    (void)h->drv->Uninitialize(h->pin);
    h->in_use = false;
}

#endif /* ALP_HAS_ALIF_HAL */
