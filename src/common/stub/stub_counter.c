/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Counter (+ quadrature encoder) NOSUPPORT stubs -- <alp/counter.h>.
 * Split out of the former src/common/stub_backend.c monolith (issue
 * #673); owns every `alp_counter_*` / `alp_qenc_*` symbol not
 * provided by a vendor backend.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/counter.h"
#include "alp/peripheral.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_COUNTER)
alp_counter_t *alp_counter_open(const alp_counter_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_counter_start(alp_counter_t *c)
{
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_counter_stop(alp_counter_t *c)
{
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_counter_get_value(alp_counter_t *c, uint32_t *t)
{
	(void)c;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_counter_us_to_ticks(alp_counter_t *c, uint32_t u, uint32_t *t)
{
	(void)c;
	(void)u;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_counter_set_alarm(alp_counter_t *c, uint32_t t, alp_counter_alarm_cb_t cb, void *u)
{
	(void)c;
	(void)t;
	(void)cb;
	(void)u;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_counter_cancel_alarm(alp_counter_t *c)
{
	(void)c;
	return ALP_ERR_NOSUPPORT;
}
void alp_counter_close(alp_counter_t *c)
{
	(void)c;
}
const alp_capabilities_t *alp_counter_capabilities(const alp_counter_t *c)
{
	(void)c;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_COUNTER */

alp_qenc_t *alp_qenc_open(const alp_qenc_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_qenc_get_position(alp_qenc_t *e, int32_t *p)
{
	(void)e;
	(void)p;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_qenc_reset_position(alp_qenc_t *e)
{
	(void)e;
	return ALP_ERR_NOSUPPORT;
}
void alp_qenc_close(alp_qenc_t *e)
{
	(void)e;
}
const alp_capabilities_t *alp_qenc_capabilities(const alp_qenc_t *e)
{
	(void)e;
	return NULL;
}
