/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPIO NOSUPPORT stubs -- <alp/peripheral.h>.  Split out of the
 * former src/common/stub_backend.c monolith (issue #673); owns every
 * `alp_gpio_*` symbol not provided by a vendor backend.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_GPIO)
alp_gpio_t *alp_gpio_open(uint32_t pin_id)
{
	(void)pin_id;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_gpio_configure(alp_gpio_t *p, alp_gpio_dir_t d, alp_gpio_pull_t pu)
{
	(void)p;
	(void)d;
	(void)pu;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_gpio_write(alp_gpio_t *p, bool l)
{
	(void)p;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_gpio_read(alp_gpio_t *p, bool *l)
{
	(void)p;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_gpio_irq_enable(alp_gpio_t *p, alp_gpio_edge_t e, alp_gpio_cb_t cb, void *u)
{
	(void)p;
	(void)e;
	(void)cb;
	(void)u;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_gpio_irq_disable(alp_gpio_t *p)
{
	(void)p;
	return ALP_ERR_NOSUPPORT;
}
void alp_gpio_close(alp_gpio_t *p)
{
	(void)p;
}
#endif /* !ALP_VENDOR_OVERRIDES_GPIO */

/* Unguarded -- same reasoning as alp_i2c_capabilities: no Linux
 * gpiochip wrapper or plain stub implements it; only the Zephyr registry
 * dispatcher (src/gpio_dispatch.c) does (#593). */
const alp_capabilities_t *alp_gpio_capabilities(const alp_gpio_t *p)
{
	(void)p;
	return NULL;
}
