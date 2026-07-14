/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e -- TI Wi-Fi 6 + BLE 5.4 coprocessor (E1M-AEN companion MCU).
 * NULL-arg validation + post-init NOT_READY contract.
 */

#include <zephyr/ztest.h>

#include "alp/chips/cc3501e.h"
#include "alp/e1m_pinout.h"
#include "alp/peripheral.h"

ZTEST(alp_chips, test_cc3501e_init_null_args)
{
	cc3501e_t  ctx;
	alp_spi_t *bus = alp_spi_open(&(alp_spi_config_t){
	    .bus_id        = 0u,
	    .freq_hz       = 10000000u,
	    .mode          = ALP_SPI_MODE_0,
	    .bits_per_word = 8u,
	    .cs_pin_id     = 0u,
	});
	/* The test rig's SPI emul may or may not be available; either
     * way the NULL-arg paths are testable. */
	zassert_equal(cc3501e_init(NULL, bus), ALP_ERR_INVAL);
	zassert_equal(cc3501e_init(&ctx, NULL), ALP_ERR_INVAL);
	if (bus != NULL) alp_spi_close(bus);
}

ZTEST(alp_chips, test_cc3501e_calls_reject_uninitialised)
{
	cc3501e_t ctx = { 0 };
	uint16_t  version;
	uint8_t   tx[4]  = { 0 }, rx[4];
	size_t    rx_len = sizeof rx;

	zassert_equal(cc3501e_reset(&ctx), ALP_ERR_NOT_READY);
	zassert_equal(cc3501e_get_version(&ctx, &version), ALP_ERR_NOT_READY);
	zassert_equal(
	    cc3501e_request(&ctx, (alp_cc3501e_cmd_t)0, tx, sizeof tx, rx, sizeof rx, &rx_len, 100u),
	    ALP_ERR_NOT_READY);
	zassert_equal(cc3501e_set_event_callback(&ctx, NULL, NULL), ALP_ERR_NOT_READY);
}
