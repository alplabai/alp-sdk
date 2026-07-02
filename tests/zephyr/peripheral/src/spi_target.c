/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/peripheral.h> -- SPI target (slave) mode binding-layer tests.
 * First native_sim coverage for the v0.9 alp_spi_target_* surface:
 * open-time config validation (mode / bits_per_word), the
 * transceive argument contract (len == 0 asymmetry vs controller
 * mode, NULL-handle guard), the bounded-wait NOSUPPORT degrade on a
 * sync-only build (CONFIG_SPI_ASYNC deliberately off in prj.conf),
 * and close idempotence.  Never calls the unbounded
 * (UINT32_MAX-timeout) wait -- nothing external clocks the emulated
 * bus, so that would hang the harness.
 */

#include <stdint.h>

#include <zephyr/ztest.h>

#include "alp/peripheral.h"

static alp_spi_target_config_t _valid_cfg(void)
{
	return (alp_spi_target_config_t){
		.bus_id        = 0,
		.mode          = ALP_SPI_MODE_0,
		.bits_per_word = 8,
	};
}

ZTEST(alp_peripheral, test_spi_target_null_cfg_returns_null)
{
	zassert_is_null(alp_spi_target_open(NULL));
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_spi_target_bad_mode_inval)
{
	alp_spi_target_config_t cfg = _valid_cfg();
	cfg.mode                    = (alp_spi_mode_t)4; /* only modes 0..3 exist */
	zassert_is_null(alp_spi_target_open(&cfg));
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_spi_target_bad_bits_per_word_inval)
{
	alp_spi_target_config_t cfg = _valid_cfg();
	cfg.bits_per_word           = 33; /* contract tops out at 32 (0 = default 8) */
	zassert_is_null(alp_spi_target_open(&cfg));
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_spi_target_invalid_bus_returns_null)
{
	alp_spi_target_config_t cfg = _valid_cfg();
	cfg.bus_id                  = 99;
	zassert_is_null(alp_spi_target_open(&cfg), "out-of-range bus_id must yield NULL");
	/* 99 exceeds the backend's static bus table (INVAL) as well as
	 * the SoC's documented SPI count (OUT_OF_RANGE) -- either precise
	 * refusal is acceptable; what matters is NO handle. */
	zassert_true(alp_last_error() == ALP_ERR_INVAL || alp_last_error() == ALP_ERR_OUT_OF_RANGE,
	             "got %d", (int)alp_last_error());
}

ZTEST(alp_peripheral, test_spi_target_transceive_null_handle_not_ready)
{
	uint8_t buf[4] = { 0 };
	size_t  got    = 99;
	zassert_equal(alp_spi_target_transceive(NULL, buf, buf, sizeof buf, &got, 100), ALP_ERR_NOT_READY);
	zassert_equal(got, 0, "rx_len must be zeroed on every failure path");
}

ZTEST(alp_peripheral, test_spi_target_transceive_arg_validation)
{
	alp_spi_target_config_t cfg = _valid_cfg();
	alp_spi_target_t       *tgt = alp_spi_target_open(&cfg);
	zassert_not_null(tgt, "emulated alp-spi0 should open in slave mode (last_error=%d)",
	                 (int)alp_last_error());

	uint8_t buf[4] = { 0 };
	size_t  got    = 99;

	/* len == 0 is INVAL in target mode (a slave cannot stage a
	 * zero-length transfer) -- deliberately asymmetric with the
	 * controller-mode alp_spi_transceive no-op. */
	zassert_equal(alp_spi_target_transceive(tgt, buf, buf, 0, &got, 100), ALP_ERR_INVAL);
	zassert_equal(got, 0);

	/* Both directions NULL: nothing to transfer either way. */
	zassert_equal(alp_spi_target_transceive(tgt, NULL, NULL, sizeof buf, &got, 100),
	              ALP_ERR_INVAL);

	zassert_equal(alp_spi_target_close(tgt), ALP_OK);
}

ZTEST(alp_peripheral, test_spi_target_bounded_wait_nosupport_without_async)
{
	/* This build compiles the Zephyr backend WITHOUT CONFIG_SPI_ASYNC
	 * (see prj.conf), so a finite timeout cannot be honoured.  The
	 * contract demands an immediate ALP_ERR_NOSUPPORT rather than
	 * silently blocking forever -- this is the bounded-wait degrade
	 * path apps must handle. */
	alp_spi_target_config_t cfg = _valid_cfg();
	alp_spi_target_t       *tgt = alp_spi_target_open(&cfg);
	zassert_not_null(tgt, "emulated alp-spi0 should open in slave mode (last_error=%d)",
	                 (int)alp_last_error());

	uint8_t      buf[4] = { 0 };
	size_t       got    = 99;
	alp_status_t s      = alp_spi_target_transceive(tgt, buf, buf, sizeof buf, &got, 100);
	zassert_equal(s, ALP_ERR_NOSUPPORT, "finite timeout on sync-only build: got %d", (int)s);
	zassert_equal(got, 0);

	zassert_equal(alp_spi_target_close(tgt), ALP_OK);
}

ZTEST(alp_peripheral, test_spi_target_close_idempotent)
{
	/* NULL and double-close are both harmless no-ops returning OK. */
	zassert_equal(alp_spi_target_close(NULL), ALP_OK);

	alp_spi_target_config_t cfg = _valid_cfg();
	alp_spi_target_t       *tgt = alp_spi_target_open(&cfg);
	zassert_not_null(tgt);
	zassert_equal(alp_spi_target_close(tgt), ALP_OK);
	zassert_equal(alp_spi_target_close(tgt), ALP_OK, "double close must stay a no-op");
}
