/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Allegro A4988 stepper driver.  See <alp/chips/a4988.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/a4988.h"

alp_status_t a4988_init(a4988_t    *dev,
                        alp_pwm_t  *step,
                        alp_gpio_t *dir,
                        alp_gpio_t *nenable,
                        alp_gpio_t *ms1,
                        alp_gpio_t *ms2,
                        alp_gpio_t *ms3)
{
    if (dev == NULL || step == NULL || dir == NULL) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->step    = step;
    dev->dir     = dir;
    dev->nenable = nenable;
    dev->ms1     = ms1;
    dev->ms2     = ms2;
    dev->ms3     = ms3;

    (void)alp_pwm_set_duty(step, 0u);
    (void)alp_gpio_write(dir, false);
    if (nenable != NULL) (void)alp_gpio_write(nenable, false);

    dev->initialised = true;
    return ALP_OK;
}

alp_status_t a4988_set_microstep(a4988_t *dev, a4988_ustep_t mode)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (dev->ms1 == NULL || dev->ms2 == NULL || dev->ms3 == NULL) return ALP_ERR_NOSUPPORT;
    if (mode != A4988_USTEP_FULL && mode != A4988_USTEP_HALF && mode != A4988_USTEP_1_4 &&
        mode != A4988_USTEP_1_8 && mode != A4988_USTEP_1_16)
        return ALP_ERR_INVAL;
    alp_status_t s = alp_gpio_write(dev->ms1, ((uint8_t)mode & 0x01u) != 0u);
    if (s != ALP_OK) return s;
    s = alp_gpio_write(dev->ms2, ((uint8_t)mode & 0x02u) != 0u);
    if (s != ALP_OK) return s;
    return alp_gpio_write(dev->ms3, ((uint8_t)mode & 0x04u) != 0u);
}

alp_status_t a4988_set_direction(a4988_t *dev, bool forward)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    return alp_gpio_write(dev->dir, forward);
}

alp_status_t a4988_set_step_rate(a4988_t *dev, uint32_t freq_hz)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (freq_hz > 250000u) return ALP_ERR_INVAL;
    if (freq_hz == 0u) return alp_pwm_set_duty(dev->step, 0u);
    const uint32_t period_ns = 1000000000u / freq_hz;
    alp_status_t   s         = alp_pwm_set_period(dev->step, period_ns);
    if (s != ALP_OK) return s;
    return alp_pwm_set_duty(dev->step, period_ns / 2u);
}

alp_status_t a4988_set_enable(a4988_t *dev, bool enabled)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (dev->nenable == NULL) return ALP_ERR_NOSUPPORT;
    return alp_gpio_write(dev->nenable, !enabled); /* active-low enable */
}

void a4988_deinit(a4988_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->step        = NULL;
    dev->dir         = NULL;
    dev->nenable     = NULL;
    dev->ms1         = NULL;
    dev->ms2         = NULL;
    dev->ms3         = NULL;
}
