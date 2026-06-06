/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * TI DRV8825 stepper driver.  See <alp/chips/drv8825.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/drv8825.h"

alp_status_t drv8825_init(drv8825_t  *dev,
                          alp_pwm_t  *step,
                          alp_gpio_t *dir,
                          alp_gpio_t *nenbl,
                          alp_gpio_t *m0,
                          alp_gpio_t *m1,
                          alp_gpio_t *m2)
{
    if (dev == NULL || step == NULL || dir == NULL) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->step  = step;
    dev->dir   = dir;
    dev->nenbl = nenbl;
    dev->m0    = m0;
    dev->m1    = m1;
    dev->m2    = m2;

    (void)alp_pwm_set_duty(step, 0u);
    (void)alp_gpio_write(dir, false);
    if (nenbl != NULL) (void)alp_gpio_write(nenbl, false); /* enable */

    dev->initialised = true;
    return ALP_OK;
}

alp_status_t drv8825_set_microstep(drv8825_t *dev, drv8825_ustep_t mode)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if ((int)mode < 0 || (int)mode > (int)DRV8825_USTEP_1_32) return ALP_ERR_INVAL;
    if (dev->m0 == NULL || dev->m1 == NULL || dev->m2 == NULL) return ALP_ERR_NOSUPPORT;
    alp_status_t s = alp_gpio_write(dev->m0, ((uint8_t)mode & 0x01u) != 0u);
    if (s != ALP_OK) return s;
    s = alp_gpio_write(dev->m1, ((uint8_t)mode & 0x02u) != 0u);
    if (s != ALP_OK) return s;
    return alp_gpio_write(dev->m2, ((uint8_t)mode & 0x04u) != 0u);
}

alp_status_t drv8825_set_direction(drv8825_t *dev, bool forward)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    return alp_gpio_write(dev->dir, forward);
}

alp_status_t drv8825_set_step_rate(drv8825_t *dev, uint32_t freq_hz)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (freq_hz > 250000u) return ALP_ERR_INVAL;
    if (freq_hz == 0u) return alp_pwm_set_duty(dev->step, 0u);
    /* period_ns = 1_000_000_000 / freq_hz; duty = period / 2. */
    const uint32_t period_ns = 1000000000u / freq_hz;
    alp_status_t   s         = alp_pwm_set_period(dev->step, period_ns);
    if (s != ALP_OK) return s;
    return alp_pwm_set_duty(dev->step, period_ns / 2u);
}

alp_status_t drv8825_set_sleep(drv8825_t *dev, bool sleeping)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (dev->nenbl == NULL) return ALP_ERR_NOSUPPORT;
    return alp_gpio_write(dev->nenbl, sleeping); /* nENBL high disables */
}

void drv8825_deinit(drv8825_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->step        = NULL;
    dev->dir         = NULL;
    dev->nenbl       = NULL;
    dev->m0          = NULL;
    dev->m1          = NULL;
    dev->m2          = NULL;
}
