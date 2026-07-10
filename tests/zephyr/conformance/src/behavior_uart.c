/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Behavioral suite for the alp/testing UART virtual backend
 * (epic #610 PR2 -- the first virtual-clock TIMEOUT consumer).
 * Compiled only for the alp_sdk.conformance.test_doubles twister
 * scenario (CONFIG_ALP_SDK_TESTING=y) -- see testcase.yaml.  Drives
 * the double through the PUBLIC alp/peripheral.h API plus the
 * alp/testing injection surface; never touches uart_ops.h /
 * testing_drv.c internals directly.
 *
 * Built alongside src/behavior_gpio.c in this scenario's app image
 * (this scenario's CMakeLists swaps BOTH behavior_*.c files in for
 * src/main.c) -- see the top of testing/behavior_gpio.c for why
 * main.c's per-class expectations are incompatible with a
 * priority-255 "open ANY instance" test double and must never share a
 * binary with it.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>
#include <alp/testing/clock.h>
#include <alp/testing/common.h>
#include <alp/testing/uart.h>

/* Local copy of main.c's / behavior_gpio.c's enum-membership helper --
 * both are static to their own TU. */
static bool status_in_enum(alp_status_t s)
{
	return s <= ALP_OK && s >= ALP_STATUS_ENUM_FLOOR;
}

static alp_uart_t *open_port(uint32_t port_id)
{
	alp_uart_config_t cfg = ALP_UART_CONFIG_DEFAULT(port_id);
	return alp_uart_open(&cfg);
}

static void uart_behavior_before(void *fixture)
{
	ARG_UNUSED(fixture);
	alp_testing_reset_all();
}

/* After-each teardown, mirroring behavior_gpio.c's dispatcher
 * pool-health check: alp_testing_reset_all() wipes the testing
 * double's own state but cannot reach the DISPATCHER's private static
 * handle pool (CONFIG_ALP_SDK_MAX_UART_HANDLES slots,
 * src/uart_dispatch.c).  A test that leaks a handle would otherwise
 * silently shrink the pool for every later test until it quietly runs
 * out, surfacing as a confusing ALP_ERR_NOMEM far from the actual
 * leak.  Round-tripping a fresh handle here fails loudly instead. */
static void uart_behavior_after(void *fixture)
{
	ARG_UNUSED(fixture);
	alp_testing_reset_all();

	alp_uart_t *h = open_port(0);
	zassert_not_null(h,
	                 "pool-health check failed: alp_uart_open(0) returned NULL right after "
	                 "this test -- a prior test in this file leaked a handle out of the "
	                 "dispatcher's fixed-size pool");
	alp_uart_close(h);
}

ZTEST_SUITE(alp_testing_uart_behavior, NULL, NULL, uart_behavior_before, uart_behavior_after, NULL);

/* Setup-fixture-shaped assertion (mirrors behavior_gpio.c's): a
 * mis-selection must fail LOUDLY, not silently exercise the wrong
 * backend for every other case below. */
ZTEST(alp_testing_uart_behavior, test_backend_selection_is_the_test_double)
{
	const alp_backend_t *be = alp_backend_select("uart", ALP_SOC_REF_STR);

	zassert_not_null(be, "uart class has no registered backend at all");
	zassert_equal(be->priority,
	              255,
	              "uart backend selection picked priority %u, not the "
	              "reserved test-double priority 255 -- CONFIG_ALP_SDK_TESTING_UART "
	              "not set, or a higher-priority backend was added",
	              (unsigned)be->priority);
	zassert_equal(strcmp(be->vendor, "alp_testing"),
	              0,
	              "uart backend selection picked vendor '%s', not 'alp_testing'",
	              be->vendor);
}

ZTEST(alp_testing_uart_behavior, test_rx_feed_read_round_trip)
{
	const uint32_t port_id = 10;
	const uint8_t  msg[]   = { 'h', 'i' };

	zassert_equal(alp_testing_uart_rx_feed(port_id, msg, sizeof(msg)), ALP_OK, "rx_feed failed");

	alp_uart_t *h = open_port(port_id);
	zassert_not_null(h, "uart test double must open ANY instance");

	uint8_t buf[2] = { 0 };
	zassert_equal(alp_uart_read(h, buf, sizeof(buf), 100), ALP_OK, "read() failed");
	zassert_mem_equal(buf, msg, sizeof(msg), "read() did not observe the fed bytes");

	alp_uart_close(h);
}

ZTEST(alp_testing_uart_behavior, test_tx_write_drain_round_trip)
{
	const uint32_t port_id = 11;
	const uint8_t  msg[]   = { 'o', 'k' };

	alp_uart_t *h = open_port(port_id);
	zassert_not_null(h, "uart test double must open ANY instance");

	zassert_equal(alp_uart_write(h, msg, sizeof(msg)), ALP_OK, "write() failed");

	uint8_t out[8]  = { 0 };
	size_t  drained = alp_testing_uart_tx_drain(port_id, out, sizeof(out));
	zassert_equal(
	    drained, sizeof(msg), "tx_drain returned %zu, expected %zu", drained, sizeof(msg));
	zassert_mem_equal(out, msg, sizeof(msg), "tx_drain did not observe the written bytes");

	/* Drained means drained: a second drain sees nothing more. */
	zassert_equal(alp_testing_uart_tx_drain(port_id, out, sizeof(out)),
	              0,
	              "tx_drain must return 0 once nothing new has been written");

	alp_uart_close(h);
}

/* Timeout boundary, lower edge: a deferred feed due strictly BEFORE
 * the deadline is satisfiable -- read() must succeed and advance the
 * virtual clock to exactly the delivery timestamp, not the full
 * timeout window. */
ZTEST(alp_testing_uart_behavior, test_timeout_boundary_delivered_before_deadline)
{
	const uint32_t port_id    = 12;
	const uint8_t  byte       = 0x42;
	const uint64_t timeout_ms = 100;
	const uint64_t at_ms      = alp_testing_clock_now_ms() + timeout_ms - 1; /* T - epsilon */

	zassert_equal(
	    alp_testing_uart_rx_feed_at(port_id, at_ms, &byte, 1), ALP_OK, "rx_feed_at failed");

	alp_uart_t *h = open_port(port_id);
	zassert_not_null(h, "uart test double must open ANY instance");

	uint8_t got = 0;
	zassert_equal(alp_uart_read(h, &got, 1, (uint32_t)timeout_ms),
	              ALP_OK,
	              "read() must succeed within the window");
	zassert_equal(got, byte, "read() did not observe the deferred byte");
	zassert_equal(alp_testing_clock_now_ms(),
	              at_ms,
	              "virtual clock must advance to the exact delivery timestamp, not the full "
	              "timeout window");

	alp_uart_close(h);
}

/* Timeout boundary, upper edge: a deferred feed due strictly AFTER the
 * deadline is unreachable within the window -- read() must time out
 * and advance the virtual clock by the FULL timeout_ms. */
ZTEST(alp_testing_uart_behavior, test_timeout_boundary_delivered_after_deadline)
{
	const uint32_t port_id    = 13;
	const uint8_t  byte       = 0x99;
	const uint64_t timeout_ms = 100;
	const uint64_t start      = alp_testing_clock_now_ms();
	const uint64_t at_ms      = start + timeout_ms + 1; /* T + epsilon */

	zassert_equal(
	    alp_testing_uart_rx_feed_at(port_id, at_ms, &byte, 1), ALP_OK, "rx_feed_at failed");

	alp_uart_t *h = open_port(port_id);
	zassert_not_null(h, "uart test double must open ANY instance");

	uint8_t got = 0;
	zassert_equal(
	    alp_uart_read(h, &got, 1, (uint32_t)timeout_ms), ALP_ERR_TIMEOUT, "read() must time out");
	zassert_equal(alp_testing_clock_now_ms(),
	              start + timeout_ms,
	              "virtual clock must advance by exactly the full timeout_ms on a timeout");

	alp_uart_close(h);
}

/* Partial/short read: fewer bytes are queued than requested, and
 * nothing more is forthcoming within the window -- alp_uart_read()
 * documents ALP_OK with the partial data whenever at least one byte
 * arrived before the deadline. */
ZTEST(alp_testing_uart_behavior, test_partial_short_read)
{
	const uint32_t port_id = 14;
	const uint8_t  msg[]   = { 'a', 'b' };

	zassert_equal(alp_testing_uart_rx_feed(port_id, msg, sizeof(msg)), ALP_OK, "rx_feed failed");

	alp_uart_t *h = open_port(port_id);
	zassert_not_null(h, "uart test double must open ANY instance");

	uint8_t buf[5] = { 0 };
	zassert_equal(alp_uart_read(h, buf, sizeof(buf), 50),
	              ALP_OK,
	              "read() must return ALP_OK on a partial fill (>0 bytes collected)");
	zassert_mem_equal(buf, msg, sizeof(msg), "short read did not deliver the queued bytes");

	alp_uart_close(h);
}

/* Injected framing error: surfaces as its documented alp_status_t,
 * not a raw errno, and is consumed at its queue position. */
ZTEST(alp_testing_uart_behavior, test_injected_error_surfaces_documented_status)
{
	const uint32_t port_id = 15;

	zassert_equal(
	    alp_testing_uart_rx_inject_error(port_id, ALP_ERR_IO), ALP_OK, "rx_inject_error failed");

	alp_uart_t *h = open_port(port_id);
	zassert_not_null(h, "uart test double must open ANY instance");

	uint8_t      buf[1] = { 0 };
	alp_status_t s      = alp_uart_read(h, buf, sizeof(buf), 10);
	zassert_equal(s, ALP_ERR_IO, "read() must surface the injected error verbatim");
	zassert_true(status_in_enum(s), "status %d outside alp_status_t", (int)s);

	/* Consumed: a following read against an empty queue times out
	 * rather than re-surfacing the same error. */
	zassert_equal(alp_uart_read(h, buf, sizeof(buf), 0),
	              ALP_ERR_TIMEOUT,
	              "the injected error must be dequeued once surfaced, not re-delivered");

	alp_uart_close(h);
}

/* An error queued BEHIND already-collected data must not be consumed
 * on the same call: the call reports OK with the partial data, and
 * the error stays queued for the next read(). */
ZTEST(alp_testing_uart_behavior, test_error_behind_data_stays_queued)
{
	const uint32_t port_id = 16;
	const uint8_t  byte    = 0x11;

	zassert_equal(alp_testing_uart_rx_feed(port_id, &byte, 1), ALP_OK, "rx_feed failed");
	zassert_equal(
	    alp_testing_uart_rx_inject_error(port_id, ALP_ERR_IO), ALP_OK, "rx_inject_error failed");

	alp_uart_t *h = open_port(port_id);
	zassert_not_null(h, "uart test double must open ANY instance");

	uint8_t buf[4] = { 0 };
	zassert_equal(alp_uart_read(h, buf, sizeof(buf), 10),
	              ALP_OK,
	              "read() must stop at the queued error and return the data collected so far");
	zassert_equal(buf[0], byte, "read() did not return the byte queued ahead of the error");

	zassert_equal(alp_uart_read(h, buf, sizeof(buf), 10),
	              ALP_ERR_IO,
	              "the queued error must surface on the NEXT read()");

	alp_uart_close(h);
}

/* Backend-selection assertion beyond the fixture's own check --
 * exercised again explicitly per the task's deliverable list. */
ZTEST(alp_testing_uart_behavior, test_backend_vendor_is_alp_testing)
{
	const alp_backend_t *be = alp_backend_select("uart", ALP_SOC_REF_STR);

	zassert_not_null(be, "uart class has no registered backend at all");
	zassert_equal(strcmp(be->vendor, "alp_testing"), 0, "vendor must be alp_testing");
}

/* Reset-frees-side-state regression: feed RX data and write TX data
 * without draining either, then reset_all() and prove the port is
 * fully reusable -- no leftover bytes, no stale error, no capacity
 * exhaustion carried across the reset. */
ZTEST(alp_testing_uart_behavior, test_reset_all_frees_side_state)
{
	const uint32_t port_id = 17;
	const uint8_t  rx_byte = 0xAA;
	const uint8_t  tx_byte = 0xBB;

	zassert_equal(alp_testing_uart_rx_feed(port_id, &rx_byte, 1), ALP_OK, "rx_feed failed");
	zassert_equal(
	    alp_testing_uart_rx_inject_error(port_id, ALP_ERR_IO), ALP_OK, "rx_inject_error failed");

	alp_uart_t *h = open_port(port_id);
	zassert_not_null(h, "uart test double must open ANY instance");
	zassert_equal(alp_uart_write(h, &tx_byte, 1), ALP_OK, "write() failed");
	alp_uart_close(h);

	alp_testing_reset_all();

	/* Port is reusable: no leftover RX byte/error, no leftover TX
	 * capture -- a fresh open+read times out and tx_drain is empty. */
	h = open_port(port_id);
	zassert_not_null(h, "port must be reusable after reset_all()");

	uint8_t buf[1] = { 0 };
	zassert_equal(alp_uart_read(h, buf, sizeof(buf), 0),
	              ALP_ERR_TIMEOUT,
	              "reset_all() must have cleared the RX queue (feed + error) for this port");

	uint8_t out[4] = { 0 };
	zassert_equal(alp_testing_uart_tx_drain(port_id, out, sizeof(out)),
	              0,
	              "reset_all() must have cleared the TX capture ring for this port");

	alp_uart_close(h);
}
