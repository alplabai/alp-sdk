/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/peripheral.h> -- SPI binding-layer tests.  §C.16 split +
 * §C.22 thin-spot fills: NULL-handle / NULL-arg guards on every
 * public transfer function so the binding layer's input-
 * validation contract is exercised on every native_sim build.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"

ZTEST(alp_peripheral, test_spi_open_close_roundtrip)
{
	alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
	    .bus_id        = 0,
	    .freq_hz       = 1000000,
	    .mode          = ALP_SPI_MODE_0,
	    .bits_per_word = 8,
	    .cs_pin_id     = 0xFFFFFFFFu, /* no CS */
	});
	zassert_not_null(spi, "alp_spi_open should succeed for bus_id=0");
	alp_spi_close(spi);
}

ZTEST(alp_peripheral, test_spi_open_invalid_bus_returns_null)
{
	alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
	    .bus_id = 99,
	});
	zassert_is_null(spi, "out-of-range bus_id must yield NULL");
}

ZTEST(alp_peripheral, test_spi_open_null_cfg_returns_null)
{
	/* §C.22: NULL cfg rejects before any handle allocation. */
	zassert_is_null(alp_spi_open(NULL));
}

ZTEST(alp_peripheral, test_spi_transceive_null_handle_not_ready)
{
	/* §C.22: half- and full-duplex transfer functions all guard
     * NULL handles uniformly. */
	uint8_t tx[4] = { 0xAA, 0x55, 0xCC, 0x33 };
	uint8_t rx[4] = { 0 };
	zassert_equal(alp_spi_transceive(NULL, tx, rx, sizeof tx), ALP_ERR_NOT_READY);
}

ZTEST(alp_peripheral, test_spi_write_null_handle_not_ready)
{
	uint8_t tx[4] = { 0xAA, 0x55, 0xCC, 0x33 };
	zassert_equal(alp_spi_write(NULL, tx, sizeof tx), ALP_ERR_NOT_READY);
}

ZTEST(alp_peripheral, test_spi_read_null_handle_not_ready)
{
	uint8_t rx[4] = { 0 };
	zassert_equal(alp_spi_read(NULL, rx, sizeof rx), ALP_ERR_NOT_READY);
}

ZTEST(alp_peripheral, test_spi_close_null_is_noop)
{
	alp_spi_close(NULL);
}
