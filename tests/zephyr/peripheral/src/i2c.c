/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/peripheral.h> -- I2C binding-layer smoke tests.  Extracted from
 * main.c in §C.16 (per-peripheral split).  Coverage focus is the
 * open/close lifecycle + NULL-arg validation + status-code
 * propagation on the Zephyr native_sim backend.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"

ZTEST(alp_peripheral, test_i2c_open_close_roundtrip) {
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id = 0,
        .bitrate_hz = 100000,
    });
    zassert_not_null(bus, "alp_i2c_open should succeed for bus_id=0");
    alp_i2c_close(bus);
}

ZTEST(alp_peripheral, test_i2c_open_invalid_bus_returns_null) {
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id = 99,
        .bitrate_hz = 100000,
    });
    zassert_is_null(bus, "out-of-range bus_id must yield NULL");
}

ZTEST(alp_peripheral, test_i2c_null_cfg_returns_null) {
    zassert_is_null(alp_i2c_open(NULL), "NULL cfg must yield NULL");
}

ZTEST(alp_peripheral, test_i2c_write_on_closed_handle_errors) {
    /* Calling on a NULL handle is the closest portable analogue of
     * "use-after-close" without leaking pool internals. */
    alp_status_t s = alp_i2c_write(NULL, 0x42, (uint8_t[]){0xaa}, 1);
    zassert_equal(s, ALP_ERR_NOT_READY, "got %d", (int)s);
}
