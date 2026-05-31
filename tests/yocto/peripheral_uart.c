/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plain-CMake tests for the Yocto/Linux termios backend
 * (src/yocto/peripheral_uart.c).
 *
 * Failure-path coverage only -- real-adapter testing wants a
 * loopback (RX/TX shorted) and lives behind the v0.4 hil-yocto
 * runner.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_uart
 *   ctest --test-dir build -R alp_test_peripheral_uart
 */

#include <stdint.h>

#include "alp/peripheral.h"

#include "test_assert.h"

/* /dev/ttyS999 will not exist on any sane CI runner. */
#define ALP_TEST_PORT_NONEXISTENT 999u

static void test_null_cfg_returns_null_and_stamps_invalid(void)
{
    alp_uart_t *p = alp_uart_open(NULL);
    ALP_ASSERT_NULL(p);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_invalid_data_bits_returns_null_and_stamps_invalid(void)
{
    alp_uart_config_t cfg = {
        .port_id   = 0,
        .baudrate  = 115200,
        .data_bits = 9, /* termios only accepts 5..8 */
        .stop_bits = 1,
        .parity    = ALP_UART_PARITY_NONE,
    };
    alp_uart_t *p = alp_uart_open(&cfg);
    ALP_ASSERT_NULL(p);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_invalid_stop_bits_returns_null_and_stamps_invalid(void)
{
    alp_uart_config_t cfg = {
        .port_id   = 0,
        .baudrate  = 115200,
        .data_bits = 8,
        .stop_bits = 3, /* only 1 or 2 */
        .parity    = ALP_UART_PARITY_NONE,
    };
    alp_uart_t *p = alp_uart_open(&cfg);
    ALP_ASSERT_NULL(p);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_unsupported_baud_returns_null_and_stamps_invalid(void)
{
    alp_uart_config_t cfg = {
        .port_id   = 0,
        .baudrate  = 12345u, /* not in the termios constants table */
        .data_bits = 8,
        .stop_bits = 1,
        .parity    = ALP_UART_PARITY_NONE,
    };
    alp_uart_t *p = alp_uart_open(&cfg);
    ALP_ASSERT_NULL(p);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_nonexistent_port_returns_null_and_stamps_not_ready(void)
{
    alp_uart_config_t cfg = {
        .port_id   = ALP_TEST_PORT_NONEXISTENT,
        .baudrate  = 115200,
        .data_bits = 8,
        .stop_bits = 1,
        .parity    = ALP_UART_PARITY_NONE,
    };
    alp_uart_t *p = alp_uart_open(&cfg);
    ALP_ASSERT_NULL(p);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_NOT_READY);
}

static void test_write_on_null_port_returns_invalid(void)
{
    uint8_t      buf[1] = {0x55};
    alp_status_t rc     = alp_uart_write(NULL, buf, sizeof(buf));
    ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_read_on_null_port_returns_invalid(void)
{
    uint8_t      buf[1] = {0};
    alp_status_t rc     = alp_uart_read(NULL, buf, sizeof(buf), 100u);
    ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_close_null_is_safe(void)
{
    alp_uart_close(NULL);
    ALP_TEST_PASS();
}

int main(void)
{
    test_null_cfg_returns_null_and_stamps_invalid();
    test_invalid_data_bits_returns_null_and_stamps_invalid();
    test_invalid_stop_bits_returns_null_and_stamps_invalid();
    test_unsupported_baud_returns_null_and_stamps_invalid();
    test_nonexistent_port_returns_null_and_stamps_not_ready();
    test_write_on_null_port_returns_invalid();
    test_read_on_null_port_returns_invalid();
    test_close_null_is_safe();

    ALP_TEST_SUMMARY();
}
