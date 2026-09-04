/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #1639: alp_uart_config_t.flow_control
 * on the Yocto/Linux termios backend (src/yocto/peripheral_uart.c).
 *
 * apply_flow_control() is pure (a struct termios in memory, no fd), so
 * it is exercised directly against a scratch termios struct -- same
 * #include-the-real-.c-file technique as
 * tests/yocto/peripheral_uart_closed_status.c, chosen for the same
 * reason: no CI-controllable route to a real /dev/tty* exists, and the
 * bit-level mapping this test is proving needs no fd at all.
 *
 * The end-to-end path (alp_uart_open() with a real tty rejecting a
 * flow-control request it can't honour) is bench-gated -- see issue
 * #1639's "needs-silicon" label.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_uart_flow_control
 *   ctest --test-dir build -R alp_test_peripheral_uart_flow_control
 */

#include <string.h>

#include "test_assert.h"

#include "../../src/yocto/peripheral_uart.c"

static struct termios blank_termios(void)
{
	struct termios tio;
	memset(&tio, 0, sizeof(tio));
	return tio;
}

static void test_flow_none_clears_both_mechanisms(void)
{
	struct termios tio = blank_termios();
	tio.c_cflag        = CRTSCTS;
	tio.c_iflag        = IXON | IXOFF;

	ALP_ASSERT_TRUE(apply_flow_control(ALP_UART_FLOW_NONE, &tio));
	ALP_ASSERT_TRUE((tio.c_cflag & CRTSCTS) == 0);
	ALP_ASSERT_TRUE((tio.c_iflag & (IXON | IXOFF)) == 0);
}

static void test_flow_rts_cts_sets_crtscts_only(void)
{
	struct termios tio = blank_termios();

	ALP_ASSERT_TRUE(apply_flow_control(ALP_UART_FLOW_RTS_CTS, &tio));
	ALP_ASSERT_TRUE((tio.c_cflag & CRTSCTS) == CRTSCTS);
	ALP_ASSERT_TRUE((tio.c_iflag & (IXON | IXOFF)) == 0);
}

static void test_flow_xon_xoff_sets_ixon_ixoff_only(void)
{
	struct termios tio = blank_termios();

	ALP_ASSERT_TRUE(apply_flow_control(ALP_UART_FLOW_XON_XOFF, &tio));
	ALP_ASSERT_TRUE((tio.c_iflag & (IXON | IXOFF)) == (IXON | IXOFF));
	ALP_ASSERT_TRUE((tio.c_cflag & CRTSCTS) == 0);
}

static void test_flow_unknown_enumerator_is_rejected_and_leaves_tio_untouched(void)
{
	struct termios tio = blank_termios();
	tio.c_cflag        = 0x1234u;
	tio.c_iflag        = 0x5678u;

	ALP_ASSERT_TRUE(!apply_flow_control((alp_uart_flow_t)99, &tio));
	/* Rejected before any bit is touched -- the caller (alp_uart_open)
	 * is the one that must refuse the whole open, not this helper
	 * papering over an unrecognised value with a guess. */
	ALP_ASSERT_TRUE(tio.c_cflag == 0x1234u);
	ALP_ASSERT_TRUE(tio.c_iflag == 0x5678u);
}

int main(void)
{
	test_flow_none_clears_both_mechanisms();
	test_flow_rts_cts_sets_crtscts_only();
	test_flow_xon_xoff_sets_ixon_ixoff_only();
	test_flow_unknown_enumerator_is_rejected_and_leaves_tio_untouched();

	ALP_TEST_SUMMARY();
}
