/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * I2C (controller + target) NOSUPPORT stubs -- <alp/peripheral.h>.
 * Split out of the former src/common/stub_backend.c monolith (issue
 * #673); owns every `alp_i2c_*` symbol not provided by a vendor
 * backend.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_I2C)
alp_i2c_t *alp_i2c_open(const alp_i2c_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_i2c_write(alp_i2c_t *b, uint8_t a, const uint8_t *d, size_t l)
{
	(void)b;
	(void)a;
	(void)d;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_i2c_read(alp_i2c_t *b, uint8_t a, uint8_t *d, size_t l)
{
	(void)b;
	(void)a;
	(void)d;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t
alp_i2c_write_read(alp_i2c_t *b, uint8_t a, const uint8_t *w, size_t wl, uint8_t *r, size_t rl)
{
	(void)b;
	(void)a;
	(void)w;
	(void)wl;
	(void)r;
	(void)rl;
	return ALP_ERR_NOSUPPORT;
}
void alp_i2c_close(alp_i2c_t *b)
{
	(void)b;
}
#endif /* !ALP_VENDOR_OVERRIDES_I2C */

/* Unguarded: neither the Linux i2c-dev wrapper (src/yocto/peripheral_i2c.c,
 * ALP_VENDOR_OVERRIDES_I2C=1 on that path) nor the plain stub above
 * implements alp_i2c_capabilities -- only the Zephyr registry dispatcher
 * (src/i2c_dispatch.c, never compiled outside Zephyr) does.  Compiled
 * unconditionally here so every non-Zephyr build exports the symbol (#593). */
const alp_capabilities_t *alp_i2c_capabilities(const alp_i2c_t *b)
{
	(void)b;
	return NULL;
}

/* I2C target (slave) mode -- Zephyr-only today.  Gated independently
 * of ALP_VENDOR_OVERRIDES_I2C so a vendor wrapper can adopt target
 * mode later without re-implementing the controller-mode surface. */
#if !defined(ALP_VENDOR_OVERRIDES_I2C_TARGET)
alp_i2c_target_t *alp_i2c_target_open(const alp_i2c_target_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
void alp_i2c_target_close(alp_i2c_target_t *t)
{
	(void)t;
}
#endif /* !ALP_VENDOR_OVERRIDES_I2C_TARGET */
