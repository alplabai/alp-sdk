/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2S NOSUPPORT stubs -- <alp/i2s.h>.  Split out of the former
 * src/common/stub_backend.c monolith (issue #673); owns every
 * `alp_i2s_*` symbol not provided by a vendor backend.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/i2s.h"
#include "alp/peripheral.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_I2S)
alp_i2s_t *alp_i2s_open(const alp_i2s_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_i2s_start(alp_i2s_t *i)
{
	(void)i;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_i2s_stop(alp_i2s_t *i)
{
	(void)i;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_i2s_write(alp_i2s_t *i, const void *b, size_t bytes, uint32_t t)
{
	(void)i;
	(void)b;
	(void)bytes;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_i2s_read(alp_i2s_t *i, void *b, size_t bytes, size_t *o, uint32_t t)
{
	(void)i;
	(void)b;
	(void)bytes;
	(void)o;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
void alp_i2s_close(alp_i2s_t *i)
{
	(void)i;
}
const alp_capabilities_t *alp_i2s_capabilities(const alp_i2s_t *i)
{
	(void)i;
	return NULL;
}
#endif /* !ALP_VENDOR_OVERRIDES_I2S */
