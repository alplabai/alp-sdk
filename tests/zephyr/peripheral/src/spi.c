/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/peripheral.h> -- SPI binding-layer smoke tests.  Extracted from
 * main.c in §C.16.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"

ZTEST(alp_peripheral, test_spi_open_close_roundtrip) {
    alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
        .bus_id = 0,
        .freq_hz = 1000000,
        .mode = ALP_SPI_MODE_0,
        .bits_per_word = 8,
        .cs_pin_id = 0xFFFFFFFFu,    /* no CS */
    });
    zassert_not_null(spi, "alp_spi_open should succeed for bus_id=0");
    alp_spi_close(spi);
}

ZTEST(alp_peripheral, test_spi_open_invalid_bus_returns_null) {
    alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
        .bus_id = 99,
    });
    zassert_is_null(spi, "out-of-range bus_id must yield NULL");
}
