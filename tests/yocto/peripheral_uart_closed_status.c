/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #1834: the Yocto direct-UART backend
 * (src/yocto/peripheral_uart.c) used to answer a NULL-or-closed `port`
 * with ALP_ERR_INVAL on every op, while every Zephyr dispatcher (and
 * ADR-0002's 2026-08-27 amendment) treats that as a lifecycle
 * condition and returns ALP_ERR_NOT_READY.  A closed (non-NULL,
 * in_use == false) handle is the sharper regression case -- a NULL
 * handle alone can't distinguish "never opened" from "the guard
 * degraded to INVAL", since some INVAL checks are also NULL-gated.
 *
 * This file #includes the real backend .c file directly (same
 * technique as tests/yocto/peripheral_gpio_closed_pin_status.c) to
 * reach its file-local pool_acquire()/pool_release(), which let this
 * test build a genuinely closed (non-NULL, in_use == false) handle
 * without a real /dev/tty*.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_uart_closed_status
 *   ctest --test-dir build -R alp_test_peripheral_uart_closed_status
 */

#include <stdint.h>

#include "test_assert.h"

#include "../../src/yocto/peripheral_uart.c"

/* Acquire a pool slot, then immediately release it -- in_use flips
 * back to false but the pointer stays valid (the pool is a static
 * array, not freed memory), giving a genuinely closed, non-NULL
 * handle the same shape a use-after-close from application code
 * would produce. */
static struct alp_uart *closed_port(void)
{
	struct alp_uart *h = pool_acquire();
	ALP_ASSERT_TRUE(h != NULL);
	pool_release(h);
	return h;
}

static void test_write_on_closed_port_returns_not_ready(void)
{
	uint8_t      buf[1] = { 0x55 };
	alp_status_t rc     = alp_uart_write(closed_port(), buf, sizeof(buf));
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOT_READY);
}

static void test_read_on_closed_port_returns_not_ready(void)
{
	uint8_t      buf[1] = { 0 };
	alp_status_t rc     = alp_uart_read(closed_port(), buf, sizeof(buf), 100u);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOT_READY);
}

/* The malformed-argument half of the split still answers INVAL, once
 * the handle itself is known-good -- proves the fix split the two
 * conditions apart rather than blanket-changing every INVAL to
 * NOT_READY. */
static void test_write_with_null_data_on_open_port_still_returns_inval(void)
{
	struct alp_uart *h = pool_acquire();
	ALP_ASSERT_TRUE(h != NULL);
	alp_status_t rc = alp_uart_write(h, NULL, 1);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
	pool_release(h);
}

static void test_read_with_null_data_on_open_port_still_returns_inval(void)
{
	struct alp_uart *h = pool_acquire();
	ALP_ASSERT_TRUE(h != NULL);
	alp_status_t rc = alp_uart_read(h, NULL, 1, 100u);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
	pool_release(h);
}

int main(void)
{
	test_write_on_closed_port_returns_not_ready();
	test_read_on_closed_port_returns_not_ready();
	test_write_with_null_data_on_open_port_still_returns_inval();
	test_read_with_null_data_on_open_port_still_returns_inval();

	ALP_TEST_SUMMARY();
}
