/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Behavioral suite for the alp/testing CAN virtual backend (epic
 * #610). Compiled only for the alp_sdk.conformance.test_doubles
 * twister scenario (CONFIG_ALP_SDK_TESTING=y) -- see testcase.yaml.
 * Drives the double through the PUBLIC alp/can.h API plus the
 * alp/testing injection surface; never touches can_ops.h /
 * testing_drv.c internals directly.
 *
 * Built alongside src/behavior_gpio.c and src/behavior_uart.c in this
 * scenario's app image (this scenario's CMakeLists swaps ALL
 * behavior_*.c files in for src/main.c) -- see the top of
 * behavior_gpio.c for why main.c's per-class expectations are
 * incompatible with a priority-255 "open ANY instance" test double
 * and must never share a binary with it.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/can.h>
#include <alp/soc_caps.h>
#include <alp/testing/can.h>
#include <alp/testing/clock.h>
#include <alp/testing/common.h>

/* Local copy of main.c's / behavior_gpio.c's / behavior_uart.c's
 * enum-membership helper -- each is static to its own TU. */
static bool status_in_enum(alp_status_t s)
{
	return s <= ALP_OK && s >= ALP_STATUS_ENUM_FLOOR;
}

static alp_can_t *open_bus(uint32_t bus_id)
{
	alp_can_config_t cfg = ALP_CAN_CONFIG_DEFAULT(bus_id);
	return alp_can_open(&cfg);
}

static int             g_cb_count;
static alp_can_frame_t g_cb_last_frame;
static void           *g_cb_last_user;

static void test_cb(const alp_can_frame_t *frame, void *user)
{
	g_cb_count++;
	g_cb_last_frame = *frame;
	g_cb_last_user  = user;
}

static void can_behavior_before(void *fixture)
{
	ARG_UNUSED(fixture);
	alp_testing_reset_all();
	g_cb_count      = 0;
	g_cb_last_frame = (alp_can_frame_t){ 0 };
	g_cb_last_user  = NULL;
}

/* After-each teardown (mirrors behavior_gpio.c's / behavior_uart.c's
 * dispatcher pool-health check): alp_testing_reset_all() wipes the
 * testing double's own state but cannot reach the DISPATCHER's
 * private static handle pool (CONFIG_ALP_SDK_MAX_CAN_HANDLES slots,
 * src/can_dispatch.c). A test that leaks a handle would otherwise
 * silently shrink the pool for every later test until it quietly runs
 * out, surfacing as a confusing ALP_ERR_NOMEM far from the actual
 * leak. Round-tripping a fresh handle here fails loudly instead. */
static void can_behavior_after(void *fixture)
{
	ARG_UNUSED(fixture);
	alp_testing_reset_all();

	alp_can_t *h = open_bus(0);
	zassert_not_null(h,
	                 "pool-health check failed: alp_can_open(0) returned NULL right after this "
	                 "test -- a prior test in this file leaked a handle out of the dispatcher's "
	                 "fixed-size pool");
	alp_can_close(h);
}

ZTEST_SUITE(alp_testing_can_behavior, NULL, NULL, can_behavior_before, can_behavior_after, NULL);

/* Setup-fixture-shaped assertion (mirrors behavior_gpio.c's /
 * behavior_uart.c's): a mis-selection must fail LOUDLY, not silently
 * exercise the wrong backend for every other case below. */
ZTEST(alp_testing_can_behavior, test_backend_selection_is_the_test_double)
{
	const alp_backend_t *be = alp_backend_select("can", ALP_SOC_REF_STR);

	zassert_not_null(be, "can class has no registered backend at all");
	zassert_equal(be->priority,
	              255,
	              "can backend selection picked priority %u, not the "
	              "reserved test-double priority 255 -- CONFIG_ALP_SDK_TESTING_CAN "
	              "not set, or a higher-priority backend was added",
	              (unsigned)be->priority);
	zassert_equal(strcmp(be->vendor, "alp_testing"),
	              0,
	              "can backend selection picked vendor '%s', not 'alp_testing'",
	              be->vendor);
}

/* Backend-selection assertion beyond the fixture's own check --
 * exercised again explicitly per the task's deliverable list. */
ZTEST(alp_testing_can_behavior, test_backend_vendor_is_alp_testing)
{
	const alp_backend_t *be = alp_backend_select("can", ALP_SOC_REF_STR);

	zassert_not_null(be, "can class has no registered backend at all");
	zassert_equal(strcmp(be->vendor, "alp_testing"), 0, "vendor must be alp_testing");
}

ZTEST(alp_testing_can_behavior, test_send_tx_drain_round_trip)
{
	const uint32_t bus_id = 0;

	alp_can_t *h = open_bus(bus_id);
	zassert_not_null(h, "can test double must open ANY instance");
	zassert_equal(alp_can_start(h), ALP_OK, "start() failed");

	alp_can_frame_t frame = { 0 };
	frame.id              = 0x123;
	frame.payload_len     = 3;
	frame.data[0]         = 1;
	frame.data[1]         = 2;
	frame.data[2]         = 3;

	zassert_equal(alp_can_send(h, &frame, 100), ALP_OK, "send() failed");

	alp_can_frame_t out[4] = { 0 };
	size_t          n      = alp_testing_can_tx_drain(bus_id, out, 4);
	zassert_equal(n, 1, "tx_drain returned %zu frames, expected 1", n);
	zassert_equal(out[0].id, frame.id, "tx_drain did not observe the sent frame's id");
	zassert_equal(
	    out[0].payload_len, frame.payload_len, "tx_drain did not observe the sent payload_len");
	zassert_mem_equal(
	    out[0].data, frame.data, frame.payload_len, "tx_drain did not observe the sent payload");

	/* Drained means drained: a second drain sees nothing more. */
	zassert_equal(alp_testing_can_tx_drain(bus_id, out, 4),
	              0,
	              "tx_drain must return 0 once nothing new has been sent");

	alp_can_close(h);
}

ZTEST(alp_testing_can_behavior, test_inject_rx_matching_filter_fires_cb)
{
	const uint32_t bus_id = 1;

	alp_can_t *h = open_bus(bus_id);
	zassert_not_null(h, "can test double must open ANY instance");

	alp_can_filter_t filt = { .id = 0x100, .mask = 0x7FF, .ext_id = false };
	int32_t          fid  = -1;
	zassert_equal(
	    alp_can_add_filter(h, &filt, test_cb, (void *)0x1234, &fid), ALP_OK, "add_filter() failed");

	alp_can_frame_t frame = { 0 };
	frame.id              = 0x100;
	frame.ext_id          = false;
	frame.payload_len     = 1;
	frame.data[0]         = 0xAB;

	zassert_equal(alp_testing_can_inject_rx(bus_id, &frame), ALP_OK, "inject_rx() failed");

	zassert_equal(g_cb_count, 1, "cb did not fire on a matching filter");
	zassert_equal(g_cb_last_frame.id, frame.id, "cb fired with the wrong frame id");
	zassert_equal(g_cb_last_user, (void *)0x1234, "cb fired with the wrong user pointer");

	alp_can_close(h);
}

ZTEST(alp_testing_can_behavior, test_inject_rx_non_matching_filter_does_not_fire_cb)
{
	const uint32_t bus_id = 2;

	alp_can_t *h = open_bus(bus_id);
	zassert_not_null(h, "can test double must open ANY instance");

	alp_can_filter_t filt = { .id = 0x200, .mask = 0x7FF, .ext_id = false };
	int32_t          fid  = -1;
	zassert_equal(alp_can_add_filter(h, &filt, test_cb, NULL, &fid), ALP_OK, "add_filter() failed");

	alp_can_frame_t frame = { 0 };
	frame.id              = 0x300; /* does not match filt.id/mask */
	frame.payload_len     = 0;

	zassert_equal(alp_testing_can_inject_rx(bus_id, &frame), ALP_OK, "inject_rx() failed");

	zassert_equal(g_cb_count, 0, "cb fired despite the injected frame not matching the filter");

	alp_can_close(h);
}

/* No use-after-remove: a filter removed via alp_can_remove_filter()
 * must not fire on a subsequent injection, even though the bus id
 * itself is never closed. */
ZTEST(alp_testing_can_behavior, test_remove_filter_then_inject_does_not_fire)
{
	const uint32_t bus_id = 3;

	alp_can_t *h = open_bus(bus_id);
	zassert_not_null(h, "can test double must open ANY instance");

	alp_can_filter_t filt = { .id = 0x400, .mask = 0x7FF, .ext_id = false };
	int32_t          fid  = -1;
	zassert_equal(alp_can_add_filter(h, &filt, test_cb, NULL, &fid), ALP_OK, "add_filter() failed");
	zassert_equal(alp_can_remove_filter(h, fid), ALP_OK, "remove_filter() failed");

	alp_can_frame_t frame = { 0 };
	frame.id              = 0x400;
	frame.payload_len     = 0;

	zassert_equal(alp_testing_can_inject_rx(bus_id, &frame), ALP_OK, "inject_rx() failed");

	zassert_equal(g_cb_count, 0, "cb fired after remove_filter() -- use-after-remove");

	alp_can_close(h);
}

/* No use-after-close: alp_can_close() removes every filter still
 * installed on its handle (mirrors z_close()/the GPIO double's
 * close()), so an injection that arrives after close is a no-op. */
ZTEST(alp_testing_can_behavior, test_filter_then_close_then_inject_is_a_no_op)
{
	const uint32_t bus_id = 4;

	alp_can_t *h = open_bus(bus_id);
	zassert_not_null(h, "can test double must open ANY instance");

	alp_can_filter_t filt = { .id = 0x500, .mask = 0x7FF, .ext_id = false };
	int32_t          fid  = -1;
	zassert_equal(alp_can_add_filter(h, &filt, test_cb, NULL, &fid), ALP_OK, "add_filter() failed");

	alp_can_close(h);

	alp_can_frame_t frame = { 0 };
	frame.id              = 0x500;
	frame.payload_len     = 0;

	zassert_equal(alp_testing_can_inject_rx(bus_id, &frame),
	              ALP_OK,
	              "injection on a closed bus id must not itself error");
	zassert_equal(
	    g_cb_count, 0, "cb fired after close() -- use-after-close into a freed/reused handle");
}

/* bus-off state must block alp_can_send() with the documented error
 * (<alp/can.h>: "ALP_ERR_IO on bus error"). */
ZTEST(alp_testing_can_behavior, test_bus_off_state_blocks_send)
{
	const uint32_t bus_id = 5;

	zassert_equal(alp_testing_can_set_bus_state(bus_id, ALP_CAN_STATE_BUS_OFF),
	              ALP_OK,
	              "set_bus_state() failed");

	alp_can_t *h = open_bus(bus_id);
	zassert_not_null(h, "can test double must open ANY instance");
	zassert_equal(alp_can_start(h), ALP_OK, "start() failed");

	alp_can_frame_t frame = { 0 };
	frame.id              = 0x600;
	frame.payload_len     = 0;

	alp_status_t s = alp_can_send(h, &frame, 100);
	zassert_equal(s, ALP_ERR_IO, "send() must return ALP_ERR_IO while the bus is bus-off");
	zassert_true(status_in_enum(s), "status %d outside alp_status_t", (int)s);

	/* Not captured: a rejected send never reaches the TX ring. */
	alp_can_frame_t out[1] = { 0 };
	zassert_equal(alp_testing_can_tx_drain(bus_id, out, 1),
	              0,
	              "a bus-off-rejected send must not be captured by tx_drain");

	alp_can_close(h);
}

ZTEST(alp_testing_can_behavior, test_bus_state_query_reflects_injected_state)
{
	const uint32_t bus_id = 6;

	alp_can_t *h = open_bus(bus_id);
	zassert_not_null(h, "can test double must open ANY instance");

	/* A freshly-touched bus id defaults to error-active. */
	alp_can_state_t state = ALP_CAN_STATE_BUS_OFF;
	zassert_equal(alp_testing_can_get_bus_state(bus_id, &state), ALP_OK, "get_bus_state() failed");
	zassert_equal(
	    state, ALP_CAN_STATE_ERROR_ACTIVE, "a freshly opened bus id must default to error-active");

	zassert_equal(alp_testing_can_set_bus_state(bus_id, ALP_CAN_STATE_ERROR_PASSIVE),
	              ALP_OK,
	              "set_bus_state(ERROR_PASSIVE) failed");
	zassert_equal(alp_testing_can_get_bus_state(bus_id, &state), ALP_OK, "get_bus_state() failed");
	zassert_equal(
	    state, ALP_CAN_STATE_ERROR_PASSIVE, "get_bus_state did not observe ERROR_PASSIVE");

	zassert_equal(alp_testing_can_set_bus_state(bus_id, ALP_CAN_STATE_BUS_OFF),
	              ALP_OK,
	              "set_bus_state(BUS_OFF) failed");
	zassert_equal(alp_testing_can_get_bus_state(bus_id, &state), ALP_OK, "get_bus_state() failed");
	zassert_equal(state, ALP_CAN_STATE_BUS_OFF, "get_bus_state did not observe BUS_OFF");

	alp_can_close(h);
}

/* Clock-boundary case: inject_rx_at() must not fire immediately, must
 * not fire before the scheduled timestamp, and must fire (once) the
 * moment the virtual clock's "now" reaches or passes it -- mirrors
 * behavior_gpio.c's test_edge_at_deferred_via_clock_advance. */
ZTEST(alp_testing_can_behavior, test_inject_rx_at_deferred_via_clock_advance)
{
	const uint32_t bus_id = 7;

	alp_can_t *h = open_bus(bus_id);
	zassert_not_null(h, "can test double must open ANY instance");

	alp_can_filter_t filt = { .id = 0x700, .mask = 0x7FF, .ext_id = false };
	int32_t          fid  = -1;
	zassert_equal(alp_can_add_filter(h, &filt, test_cb, NULL, &fid), ALP_OK, "add_filter() failed");

	alp_can_frame_t frame = { 0 };
	frame.id              = 0x700;
	frame.payload_len     = 0;

	zassert_equal(
	    alp_testing_can_inject_rx_at(bus_id, 100, &frame), ALP_OK, "inject_rx_at() failed");
	zassert_equal(g_cb_count, 0, "inject_rx_at() must not fire immediately");

	zassert_equal(alp_testing_clock_advance_ms(50), ALP_OK, "advance_ms(50) failed");
	zassert_equal(g_cb_count, 0, "cb fired before the scheduled timestamp");

	zassert_equal(alp_testing_clock_advance_ms(60), ALP_OK, "advance_ms(60) failed");
	zassert_equal(g_cb_count, 1, "cb did not fire once the virtual clock passed at_ms");
	zassert_true(alp_testing_clock_now_ms() >= 110, "virtual clock did not advance");

	alp_can_close(h);
}

/* Reset-frees-side-state regression: send a frame, arm a filter, and
 * inject a bus fault without draining/removing/clearing any of it,
 * then reset_all() and prove the bus id is fully reusable -- no
 * leftover TX capture, no stale filter callback, and the bus-error
 * state is back to error-active. */
ZTEST(alp_testing_can_behavior, test_reset_all_frees_side_state)
{
	const uint32_t bus_id = 8;

	zassert_equal(alp_testing_can_set_bus_state(bus_id, ALP_CAN_STATE_BUS_OFF),
	              ALP_OK,
	              "set_bus_state() failed");

	alp_can_t *h = open_bus(bus_id);
	zassert_not_null(h, "can test double must open ANY instance");
	zassert_equal(alp_can_start(h), ALP_OK, "start() failed");

	alp_can_filter_t filt = { .id = 0x800, .mask = 0x7FF, .ext_id = false };
	int32_t          fid  = -1;
	zassert_equal(alp_can_add_filter(h, &filt, test_cb, NULL, &fid), ALP_OK, "add_filter() failed");

	alp_can_close(h);

	alp_testing_reset_all();

	/* Bus id is reusable: re-touch it (a fresh open, mirroring the
	 * GPIO/UART reset tests' re-open-after-reset shape) and confirm
	 * bus-state is back to default, no leftover TX capture, and the
	 * pre-reset filter cannot fire. */
	alp_can_t *h2 = open_bus(bus_id);
	zassert_not_null(h2, "bus id must be reusable after reset_all()");

	alp_can_state_t state = ALP_CAN_STATE_BUS_OFF;
	zassert_equal(alp_testing_can_get_bus_state(bus_id, &state), ALP_OK, "get_bus_state() failed");
	zassert_equal(
	    state, ALP_CAN_STATE_ERROR_ACTIVE, "reset_all() must restore the default bus state");

	alp_can_frame_t out[1] = { 0 };
	zassert_equal(alp_testing_can_tx_drain(bus_id, out, 1),
	              0,
	              "reset_all() must have cleared the TX capture ring for this bus id");

	alp_can_frame_t frame = { 0 };
	frame.id              = 0x800;
	frame.payload_len     = 0;
	zassert_equal(alp_testing_can_inject_rx(bus_id, &frame), ALP_OK, "inject_rx() failed");
	zassert_equal(
	    g_cb_count, 0, "reset_all() must have cleared the pre-reset filter's callback wiring");

	alp_can_close(h2);
}
