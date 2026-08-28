/*
 * Copyright 2026 Alp Lab AB
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

/* NULL-or-closed `pin` is a lifecycle condition, not a malformed
 * argument -- every op below returns ALP_ERR_NOT_READY, matching
 * every Zephyr dispatcher and ADR-0002's 2026-08-27 amendment
 * (issue #1734).  This backend used to answer ALP_ERR_INVAL here,
 * indistinguishable from a real malformed-argument case; the split
 * malformed-argument checks are only ever reached once the handle
 * itself is known good, so a NULL `pin` always short-circuits to
 * NOT_READY ahead of any other check in the same call, including a
 * NULL out-param (test_read_with_null_out_and_null_pin_returns_not_ready
 * below) or edge == NONE (test_irq_enable_edge_none_and_null_pin_
 * returns_not_ready below). tests/yocto/peripheral_gpio_closed_pin_
 * status.c covers the sharper non-NULL, closed-handle case and the
 * INVAL-still-fires-on-a-good-handle counterpart, which this
 * public-API-only file can't fabricate without a real GPIO line. */

static void test_configure_on_null_pin_returns_not_ready(void)
{
	alp_status_t rc = alp_gpio_configure(NULL, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOT_READY);
}

static void test_write_on_null_pin_returns_not_ready(void)
{
	alp_status_t rc = alp_gpio_write(NULL, true);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOT_READY);
}

static void test_read_on_null_pin_returns_not_ready(void)
{
	bool         level = false;
	alp_status_t rc    = alp_gpio_read(NULL, &level);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOT_READY);
}

static void test_read_with_null_out_and_null_pin_returns_not_ready(void)
{
	alp_status_t rc = alp_gpio_read(NULL, NULL);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOT_READY);
}

static void noop_cb(alp_gpio_t *pin, void *user)
{
	(void)pin;
	(void)user;
}

static void test_irq_enable_null_pin_returns_not_ready(void)
{
	alp_status_t rc = alp_gpio_irq_enable(NULL, ALP_GPIO_EDGE_RISING, noop_cb, (void *)0);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOT_READY);
}

static void test_irq_enable_null_cb_and_null_pin_returns_not_ready(void)
{
	/* The pin-lifecycle check runs ahead of the cb-NULL check, so a
     * NULL pin wins regardless of cb. */
	alp_status_t rc = alp_gpio_irq_enable(NULL, ALP_GPIO_EDGE_RISING, NULL, (void *)0);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOT_READY);
}

static void test_irq_enable_edge_none_and_null_pin_returns_not_ready(void)
{
	/* Same short-circuit: the pin-lifecycle check runs ahead of the
     * edge == NONE check. */
	alp_status_t rc = alp_gpio_irq_enable(NULL, ALP_GPIO_EDGE_NONE, noop_cb, (void *)0);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOT_READY);
}

static void test_irq_disable_null_pin_returns_not_ready(void)
{
	alp_status_t rc = alp_gpio_irq_disable(NULL);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOT_READY);
}

static void test_close_null_is_safe(void)
{
	alp_gpio_close(NULL);
	ALP_TEST_PASS();
}

int main(void)
{
	test_nonexistent_chip_returns_null_and_stamps_not_ready();
	test_configure_on_null_pin_returns_not_ready();
	test_write_on_null_pin_returns_not_ready();
	test_read_on_null_pin_returns_not_ready();
	test_read_with_null_out_and_null_pin_returns_not_ready();
	test_irq_enable_null_pin_returns_not_ready();
	test_irq_enable_null_cb_and_null_pin_returns_not_ready();
	test_irq_enable_edge_none_and_null_pin_returns_not_ready();
	test_irq_disable_null_pin_returns_not_ready();
	test_close_null_is_safe();

	ALP_TEST_SUMMARY();
}
