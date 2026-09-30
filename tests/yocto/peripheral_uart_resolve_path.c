/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * port_id -> tty path mapping of the Yocto UART backend
 * (src/yocto/peripheral_uart.c resolve_path()), incl. the RZ/V2N SCIF
 * /dev/ttySC<N> range.  resolve_path() is pure, so the real .c file is
 * #included, same technique as peripheral_uart_flow_control.c.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_uart_resolve_path
 *   ctest --test-dir build -R alp_test_peripheral_uart_resolve_path
 */

#include <string.h>

#include "test_assert.h"

#include "../../src/yocto/peripheral_uart.c"

static void expect_path(uint32_t port_id, const char *want)
{
	char path[32];
	int  n = resolve_path(port_id, path, sizeof(path));
	ALP_ASSERT_TRUE(n > 0);
	ALP_ASSERT_TRUE(strcmp(path, want) == 0);
}

int main(void)
{
	expect_path(0u, "/dev/ttyS0");
	expect_path(99u, "/dev/ttyS99");
	expect_path(100u, "/dev/ttyAMA0");
	expect_path(199u, "/dev/ttyAMA99");
	expect_path(200u, "/dev/ttyUSB0");
	expect_path(299u, "/dev/ttyUSB99");
	expect_path(300u, "/dev/ttySC0");
	expect_path(304u, "/dev/ttySC4");
	expect_path(399u, "/dev/ttySC99");

	char path[32];
	ALP_ASSERT_TRUE(resolve_path(400u, path, sizeof(path)) < 0);
	ALP_TEST_SUMMARY();
}
