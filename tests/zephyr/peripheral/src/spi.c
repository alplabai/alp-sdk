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

/* #749: alp_spi_write / alp_spi_read are documented half-duplex APIs
 * whose buffer argument "must be non-NULL when len > 0" -- unlike
 * alp_spi_transceive, which treats a NULL tx/rx as a deliberate
 * full-duplex filler/discard convenience.  A NULL buffer with len > 0
 * must be rejected as ALP_ERR_INVAL before it can reach a real
 * transfer; NULL is still legal when len == 0 (a documented no-op). */
ZTEST(alp_peripheral, test_spi_write_rejects_null_buffer_with_nonzero_len)
{
	alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
	    .bus_id        = 0,
	    .freq_hz       = 1000000,
	    .mode          = ALP_SPI_MODE_0,
	    .bits_per_word = 8,
	    .cs_pin_id     = 0xFFFFFFFFu,
	});
	zassert_not_null(spi);

	zassert_equal(alp_spi_write(spi, NULL, 4u),
	              ALP_ERR_INVAL,
	              "NULL tx with len > 0 must be rejected, not silently transferred as 0xFF fill");
	/* len == 0 stays a legal no-op regardless of the pointer. */
	zassert_equal(alp_spi_write(spi, NULL, 0u), ALP_OK);

	alp_spi_close(spi);
}

ZTEST(alp_peripheral, test_spi_read_rejects_null_buffer_with_nonzero_len)
{
	alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
	    .bus_id        = 0,
	    .freq_hz       = 1000000,
	    .mode          = ALP_SPI_MODE_0,
	    .bits_per_word = 8,
	    .cs_pin_id     = 0xFFFFFFFFu,
	});
	zassert_not_null(spi);

	zassert_equal(
	    alp_spi_read(spi, NULL, 4u),
	    ALP_ERR_INVAL,
	    "NULL rx with len > 0 must be rejected, not silently transferred discarding MISO");
	zassert_equal(alp_spi_read(spi, NULL, 0u), ALP_OK);

	alp_spi_close(spi);
}

/* alp_spi_transceive's own NULL semantics (full-duplex filler/discard)
 * must stay untouched by the half-duplex wrappers' new guard -- a
 * direct transceive() call with NULL tx or NULL rx is still a
 * documented, legal transfer. */
ZTEST(alp_peripheral, test_spi_transceive_null_tx_or_rx_stays_legal)
{
	alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
	    .bus_id        = 0,
	    .freq_hz       = 1000000,
	    .mode          = ALP_SPI_MODE_0,
	    .bits_per_word = 8,
	    .cs_pin_id     = 0xFFFFFFFFu,
	});
	zassert_not_null(spi);

	uint8_t rx[4] = { 0 };
	zassert_not_equal(alp_spi_transceive(spi, NULL, rx, sizeof rx),
	                  ALP_ERR_INVAL,
	                  "NULL tx must still mean \"send 0xFF\" on the full-duplex primitive");
	uint8_t tx[4] = { 0xAA, 0x55, 0xCC, 0x33 };
	zassert_not_equal(alp_spi_transceive(spi, tx, NULL, sizeof tx),
	                  ALP_ERR_INVAL,
	                  "NULL rx must still mean \"discard MISO\" on the full-duplex primitive");

	alp_spi_close(spi);
}
