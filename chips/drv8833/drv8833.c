/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * TI DRV8833 dual H-bridge driver.  See <alp/chips/drv8833.h>.
 */

#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "alp/chips/drv8833.h"

static alp_status_t drv8833_set_pair(alp_pwm_t *forward, alp_pwm_t *reverse, int32_t pulse_ns)
{
	if (pulse_ns >= 0) {
		alp_status_t s = alp_pwm_set_duty(forward, (uint32_t)pulse_ns);
		if (s != ALP_OK) return s;
		return alp_pwm_set_duty(reverse, 0u);
	}
	alp_status_t s = alp_pwm_set_duty(forward, 0u);
	if (s != ALP_OK) return s;
	return alp_pwm_set_duty(reverse, (uint32_t)(-pulse_ns));
}

alp_status_t drv8833_init(drv8833_t *dev, alp_pwm_t *in1, alp_pwm_t *in2, alp_pwm_t *in3,
                          alp_pwm_t *in4, alp_gpio_t *nsleep)
{
	if (dev == NULL) return ALP_ERR_INVAL;
	if (in1 == NULL || in2 == NULL || in3 == NULL || in4 == NULL) return ALP_ERR_INVAL;
	memset(dev, 0, sizeof(*dev));
	dev->in1    = in1;
	dev->in2    = in2;
	dev->in3    = in3;
	dev->in4    = in4;
	dev->nsleep = nsleep;

	if (nsleep != NULL) {
		alp_status_t s = alp_gpio_write(nsleep, true);
		if (s != ALP_OK) return s;
	}

	/* Coast both channels at start. */
	(void)alp_pwm_set_duty(in1, 0u);
	(void)alp_pwm_set_duty(in2, 0u);
	(void)alp_pwm_set_duty(in3, 0u);
	(void)alp_pwm_set_duty(in4, 0u);

	dev->initialised = true;
	return ALP_OK;
}

alp_status_t drv8833_set_a(drv8833_t *dev, int32_t pulse_ns)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	return drv8833_set_pair(dev->in1, dev->in2, pulse_ns);
}

alp_status_t drv8833_set_b(drv8833_t *dev, int32_t pulse_ns)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	return drv8833_set_pair(dev->in3, dev->in4, pulse_ns);
}

alp_status_t drv8833_set_sleep(drv8833_t *dev, bool sleeping)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (dev->nsleep == NULL) return ALP_ERR_NOSUPPORT;
	return alp_gpio_write(dev->nsleep, !sleeping); /* active low */
}

void drv8833_deinit(drv8833_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->in1         = NULL;
	dev->in2         = NULL;
	dev->in3         = NULL;
	dev->in4         = NULL;
	dev->nsleep      = NULL;
}
