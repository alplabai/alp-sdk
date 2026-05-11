/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plain-CMake tests for the Yocto/Linux GPIO chardev-v2 backend
 * (src/yocto/peripheral_gpio.c).
 *
 * Failure-path coverage only -- real-line testing wants either
 * a loopback (output -> input on the same chip) or a known GPIO
 * fixture and lives behind the v0.4 hil-yocto runner.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_gpio
 *   ctest --test-dir build -R alp_test_peripheral_gpio
 */

#include <stdint.h>

#include "alp/peripheral.h"

#include "test_assert.h"

/* /dev/gpiochip999 won't exist on any sane CI runner.
 * pin_id encoding: (chip << 16) | line_offset. */
#define ALP_TEST_PIN_NONEXISTENT 0x03E70000u /* chip 999, line 0 */

static void test_nonexistent_chip_returns_null_and_stamps_not_ready(void)
{
    alp_gpio_t *pin = alp_gpio_open(ALP_TEST_PIN_NONEXISTENT);
    ALP_ASSERT_NULL(pin);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_NOT_READY);
}

static void test_configure_on_null_pin_returns_invalid(void)
{
    alp_status_t rc = alp_gpio_configure(NULL, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
    ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_write_on_null_pin_returns_invalid(void)
{
    alp_status_t rc = alp_gpio_write(NULL, true);
    ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_read_on_null_pin_returns_invalid(void)
{
    bool         level = false;
    alp_status_t rc    = alp_gpio_read(NULL, &level);
    ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_read_with_null_out_returns_invalid(void)
{
    alp_status_t rc = alp_gpio_read(NULL, NULL);
    ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void noop_cb(alp_gpio_t *pin, void *user)
{
    (void)pin;
    (void)user;
}

static void test_irq_enable_null_pin_returns_invalid(void)
{
    alp_status_t rc = alp_gpio_irq_enable(NULL, ALP_GPIO_EDGE_RISING, noop_cb, (void *)0);
    ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_irq_enable_null_cb_returns_invalid(void)
{
    /* Even with a NULL pin the cb-NULL check fires; cb == NULL is a
     * caller mistake regardless of the pin state. */
    alp_status_t rc = alp_gpio_irq_enable(NULL, ALP_GPIO_EDGE_RISING, NULL, (void *)0);
    ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_irq_enable_edge_none_returns_invalid(void)
{
    alp_status_t rc = alp_gpio_irq_enable(NULL, ALP_GPIO_EDGE_NONE, noop_cb, (void *)0);
    ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_irq_disable_null_pin_returns_invalid(void)
{
    alp_status_t rc = alp_gpio_irq_disable(NULL);
    ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_close_null_is_safe(void)
{
    alp_gpio_close(NULL);
    ALP_TEST_PASS();
}

int main(void)
{
    test_nonexistent_chip_returns_null_and_stamps_not_ready();
    test_configure_on_null_pin_returns_invalid();
    test_write_on_null_pin_returns_invalid();
    test_read_on_null_pin_returns_invalid();
    test_read_with_null_out_returns_invalid();
    test_irq_enable_null_pin_returns_invalid();
    test_irq_enable_null_cb_returns_invalid();
    test_irq_enable_edge_none_returns_invalid();
    test_irq_disable_null_pin_returns_invalid();
    test_close_null_is_safe();

    ALP_TEST_SUMMARY();
}
