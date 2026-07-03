/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/peripheral.h> -- I2C target (slave) mode binding-layer tests.
 * First native_sim coverage for the v0.9 alp_i2c_target_* surface:
 * config validation (NULL cfg / missing callbacks / reserved 7-bit
 * addresses), the open/close lifecycle against the emulated
 * controller (CONFIG_I2C_TARGET accepts the registration; nothing
 * external ever drives it), and close idempotence.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"

/* Minimal valid callbacks -- native_sim never fires them (no external
 * controller drives the emulated bus). */
static void _on_write(uint8_t byte, void *user)
{
	ARG_UNUSED(byte);
	ARG_UNUSED(user);
}

static alp_status_t _on_read(uint8_t *byte, void *user)
{
	ARG_UNUSED(user);
	*byte = 0x00u;
	return ALP_OK;
}

static alp_i2c_target_config_t _valid_cfg(void)
{
	return (alp_i2c_target_config_t){
		.bus_id        = 0,
		.own_addr_7bit = 0x42,
		.on_write      = _on_write,
		.on_read       = _on_read,
		.on_stop       = NULL, /* optional */
		.user          = NULL,
	};
}

ZTEST(alp_peripheral, test_i2c_target_null_cfg_returns_null)
{
	zassert_is_null(alp_i2c_target_open(NULL));
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_i2c_target_missing_callbacks_inval)
{
	alp_i2c_target_config_t cfg = _valid_cfg();
	cfg.on_write                = NULL;
	zassert_is_null(alp_i2c_target_open(&cfg), "on_write is required");
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);

	cfg         = _valid_cfg();
	cfg.on_read = NULL;
	zassert_is_null(alp_i2c_target_open(&cfg), "on_read is required");
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_i2c_target_reserved_addresses_inval)
{
	/* 0x00-0x07 (general call, CBUS, ...) and 0x78-0x7F (10-bit
	 * prefixes, device ID) are reserved by the I2C spec -- the doc
	 * contract is 0x08..0x77 only. */
	const uint8_t reserved[] = { 0x00, 0x03, 0x07, 0x78, 0x7C, 0x7F };
	for (size_t i = 0; i < ARRAY_SIZE(reserved); ++i) {
		alp_i2c_target_config_t cfg = _valid_cfg();
		cfg.own_addr_7bit           = reserved[i];
		zassert_is_null(
		    alp_i2c_target_open(&cfg), "reserved addr 0x%02x must be rejected", reserved[i]);
		zassert_equal(alp_last_error(), ALP_ERR_INVAL);
	}
}

ZTEST(alp_peripheral, test_i2c_target_open_close_roundtrip)
{
	/* native_sim's emulated controller accepts the registration
	 * (CONFIG_I2C_TARGET=y in prj.conf); callbacks never fire because
	 * no external controller drives the bus. */
	alp_i2c_target_config_t cfg = _valid_cfg();
	alp_i2c_target_t       *tgt = alp_i2c_target_open(&cfg);
	zassert_not_null(
	    tgt, "emul controller should accept registration (last_error=%d)", (int)alp_last_error());
	alp_i2c_target_close(tgt);
	/* Double-close must be a harmless no-op (idempotence contract). */
	alp_i2c_target_close(tgt);
}

ZTEST(alp_peripheral, test_i2c_target_close_null_is_noop)
{
	alp_i2c_target_close(NULL);
}

ZTEST(alp_peripheral, test_i2c_target_invalid_bus_returns_null)
{
	alp_i2c_target_config_t cfg = _valid_cfg();
	cfg.bus_id                  = 99;
	zassert_is_null(alp_i2c_target_open(&cfg), "out-of-range bus_id must yield NULL");
	/* 99 exceeds the backend's static bus table (INVAL) as well as
	 * the SoC's documented I2C count (OUT_OF_RANGE) -- either precise
	 * refusal is acceptable; what matters is NO handle. */
	zassert_true(alp_last_error() == ALP_ERR_INVAL || alp_last_error() == ALP_ERR_OUT_OF_RANGE,
	             "got %d",
	             (int)alp_last_error());
}
