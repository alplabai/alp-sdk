/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smoke tests for the Zephyr ALP SDK backend (peripheral.h).
 * Runs under native_sim with Zephyr's emulated I2C/SPI/GPIO drivers.
 *
 * Coverage focus is the *binding layer*: open/close lifecycle, status
 * code propagation, NULL-arg validation.  Per-vendor transfer
 * correctness is covered by the per-block bring-up tests in alp-studio.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"

ZTEST_SUITE(alp_peripheral, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------ */
/* I2C                                                                 */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* SPI                                                                 */
/* ------------------------------------------------------------------ */

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

/* ------------------------------------------------------------------ */
/* GPIO                                                                */
/* ------------------------------------------------------------------ */

ZTEST(alp_peripheral, test_gpio_output_write_read_roundtrip) {
    alp_gpio_t *p = alp_gpio_open(0);
    zassert_not_null(p, "alp_gpio_open(0) should succeed");

    zassert_equal(alp_gpio_configure(p, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE),
                  ALP_OK, "configure as output failed");
    zassert_equal(alp_gpio_write(p, true), ALP_OK, "write high failed");

    bool level = false;
    zassert_equal(alp_gpio_read(p, &level), ALP_OK, "read failed");
    zassert_true(level, "expected pin to read back high");

    alp_gpio_close(p);
}

ZTEST(alp_peripheral, test_gpio_invalid_pin_returns_null) {
    zassert_is_null(alp_gpio_open(99), "out-of-range pin_id must yield NULL");
}

ZTEST(alp_peripheral, test_gpio_irq_invalid_args) {
    alp_gpio_t *p = alp_gpio_open(1);
    zassert_not_null(p);
    zassert_equal(alp_gpio_irq_enable(p, ALP_GPIO_EDGE_NONE, NULL, NULL),
                  ALP_ERR_INVAL, "edge=NONE+cb=NULL must be invalid");
    alp_gpio_close(p);
}

/* ------------------------------------------------------------------ */
/* UART                                                                */
/* ------------------------------------------------------------------ */

ZTEST(alp_peripheral, test_uart_open_close_roundtrip) {
    alp_uart_t *u = alp_uart_open(&(alp_uart_config_t){
        .port_id = 0,
        .baudrate = 115200,
        .data_bits = 8,
        .stop_bits = 1,
        .parity = ALP_UART_PARITY_NONE,
    });
    zassert_not_null(u, "alp_uart_open should succeed for port_id=0");
    alp_uart_close(u);
}

ZTEST(alp_peripheral, test_uart_invalid_port_returns_null) {
    alp_uart_t *u = alp_uart_open(&(alp_uart_config_t){.port_id = 99});
    zassert_is_null(u, "out-of-range port_id must yield NULL");
}

/* ------------------------------------------------------------------ */
/* Pool exhaustion                                                     */
/* ------------------------------------------------------------------ */

ZTEST(alp_peripheral, test_gpio_pool_exhaustion_returns_null) {
    alp_gpio_t *pins[CONFIG_ALP_SDK_MAX_GPIO_HANDLES + 1] = {0};
    size_t opened = 0;

    for (size_t i = 0; i < ARRAY_SIZE(pins); i++) {
        /* Pin id 0 is valid — every claim hits the pool, regardless
         * of whether the underlying gpio is shared. */
        pins[i] = alp_gpio_open(0);
        if (pins[i] == NULL) break;
        opened++;
    }

    zassert_equal(opened, (size_t)CONFIG_ALP_SDK_MAX_GPIO_HANDLES,
                  "pool should hand out exactly CONFIG_ALP_SDK_MAX_GPIO_HANDLES "
                  "before refusing; opened=%zu", opened);

    for (size_t i = 0; i < opened; i++) {
        alp_gpio_close(pins[i]);
    }
}
