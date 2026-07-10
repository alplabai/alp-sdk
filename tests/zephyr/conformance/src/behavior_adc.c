/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Behavioral suite for the alp/testing ADC virtual backend (epic
 * #610).  Compiled only for the alp_sdk.conformance.test_doubles
 * twister scenario (CONFIG_ALP_SDK_TESTING=y) -- see testcase.yaml.
 * Drives the double through the PUBLIC alp/adc.h API plus the
 * alp/testing injection surface; never touches adc_ops.h /
 * testing_drv.c internals directly.
 *
 * Built alongside src/behavior_gpio.c and src/behavior_uart.c in this
 * scenario's app image (this scenario's CMakeLists swaps all
 * behavior_*.c files in for src/main.c) -- see the top of
 * behavior_gpio.c for why main.c's per-class expectations are
 * incompatible with a priority-255 "open ANY instance" test double
 * and must never share a binary with it.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <alp/adc.h>
#include <alp/backend.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>
#include <alp/testing/adc.h>
#include <alp/testing/common.h>

/* Local copy of main.c's / the other behavior_*.c files' enum-
 * membership helper -- all are static to their own TU. */
static bool status_in_enum(alp_status_t s)
{
	return s <= ALP_OK && s >= ALP_STATUS_ENUM_FLOOR;
}

static alp_adc_t *open_channel(uint32_t channel_id)
{
	alp_adc_config_t cfg = ALP_ADC_CONFIG_DEFAULT(channel_id);
	return alp_adc_open(&cfg);
}

static void adc_behavior_before(void *fixture)
{
	ARG_UNUSED(fixture);
	alp_testing_reset_all();
}

/* After-each teardown, mirroring behavior_gpio.c's / behavior_uart.c's
 * dispatcher pool-health check: alp_testing_reset_all() wipes the
 * testing double's own state but cannot reach the DISPATCHER's
 * private static handle pool (CONFIG_ALP_SDK_ADC_HANDLE_POOL slots,
 * src/adc_dispatch.c).  A test that leaks a handle would otherwise
 * silently shrink the pool for every later test until it quietly runs
 * out, surfacing as a confusing ALP_ERR_NOMEM far from the actual
 * leak.  Round-tripping a fresh handle here fails loudly instead. */
static void adc_behavior_after(void *fixture)
{
	ARG_UNUSED(fixture);
	alp_testing_reset_all();

	alp_adc_t *h = open_channel(0);
	zassert_not_null(h,
	                 "pool-health check failed: alp_adc_open(0) returned NULL right after this "
	                 "test -- a prior test in this file leaked a handle out of the dispatcher's "
	                 "fixed-size pool");
	alp_adc_close(h);
}

ZTEST_SUITE(alp_testing_adc_behavior, NULL, NULL, adc_behavior_before, adc_behavior_after, NULL);

/* Setup-fixture-shaped assertion (mirrors the GPIO/UART doubles'): a
 * mis-selection must fail LOUDLY, not silently exercise the wrong
 * backend for every other case below. */
ZTEST(alp_testing_adc_behavior, test_backend_selection_is_the_test_double)
{
	const alp_backend_t *be = alp_backend_select("adc", ALP_SOC_REF_STR);

	zassert_not_null(be, "adc class has no registered backend at all");
	zassert_equal(be->priority,
	              255,
	              "adc backend selection picked priority %u, not the "
	              "reserved test-double priority 255 -- CONFIG_ALP_SDK_TESTING_ADC "
	              "not set, or a higher-priority backend was added",
	              (unsigned)be->priority);
	zassert_equal(strcmp(be->vendor, "alp_testing"),
	              0,
	              "adc backend selection picked vendor '%s', not 'alp_testing'",
	              be->vendor);
}

/* Queued raw sequence: reads pop in strict FIFO order. */
ZTEST(alp_testing_adc_behavior, test_queue_raw_reads_pop_in_order)
{
	const uint32_t channel_id = 10;
	const int32_t  seq[]      = { 10, 20, 30 };

	zassert_equal(
	    alp_testing_adc_queue_raw(channel_id, seq, ARRAY_SIZE(seq)), ALP_OK, "queue_raw failed");

	alp_adc_t *h = open_channel(channel_id);
	zassert_not_null(h, "adc test double must open ANY instance");

	for (size_t i = 0; i < ARRAY_SIZE(seq); ++i) {
		int32_t raw = 0;
		zassert_equal(alp_adc_read_raw(h, &raw), ALP_OK, "read_raw() failed at index %zu", i);
		zassert_equal(
		    raw, seq[i], "read_raw() returned %d, expected %d at index %zu", raw, seq[i], i);
	}

	alp_adc_close(h);
}

/* Last-value latch: once the FIFO drains, further reads keep
 * returning the LAST popped value indefinitely, like a held input. */
ZTEST(alp_testing_adc_behavior, test_last_value_latches_after_fifo_drains)
{
	const uint32_t channel_id = 11;
	const int32_t  seq[]      = { 5, 6 };

	zassert_equal(
	    alp_testing_adc_queue_raw(channel_id, seq, ARRAY_SIZE(seq)), ALP_OK, "queue_raw failed");

	alp_adc_t *h = open_channel(channel_id);
	zassert_not_null(h, "adc test double must open ANY instance");

	int32_t raw = 0;
	zassert_equal(alp_adc_read_raw(h, &raw), ALP_OK, "first read_raw() failed");
	zassert_equal(raw, seq[0], "first read_raw() did not pop the first queued value");
	zassert_equal(alp_adc_read_raw(h, &raw), ALP_OK, "second read_raw() failed");
	zassert_equal(raw, seq[1], "second read_raw() did not pop the second queued value");

	/* FIFO is now empty; every further read latches at the last
	 * popped value (6), not 0 and not an error. */
	for (int i = 0; i < 3; ++i) {
		zassert_equal(alp_adc_read_raw(h, &raw), ALP_OK, "latched read_raw() failed on iter %d", i);
		zassert_equal(
		    raw, seq[1], "read_raw() did not latch at the last popped value on iter %d", i);
	}

	alp_adc_close(h);
}

/* A channel nothing has ever queued a sample for latches at 0 --
 * documented on <alp/testing/adc.h>. */
ZTEST(alp_testing_adc_behavior, test_untouched_channel_latches_at_zero)
{
	const uint32_t channel_id = 12;

	alp_adc_t *h = open_channel(channel_id);
	zassert_not_null(h, "adc test double must open ANY instance");

	int32_t raw = 1; /* poison, must be overwritten */
	zassert_equal(alp_adc_read_raw(h, &raw), ALP_OK, "read_raw() failed");
	zassert_equal(raw, 0, "an untouched channel must latch at 0, got %d", raw);

	alp_adc_close(h);
}

/* Boundary values round-trip through read_raw(): 0 and the max code
 * for the default 12-bit resolution ((1 << 12) - 1 == 4095). */
ZTEST(alp_testing_adc_behavior, test_boundary_raw_values_round_trip)
{
	const uint32_t channel_id  = 13;
	const int32_t  max_for_12b = (1 << 12) - 1;
	const int32_t  seq[]       = { 0, max_for_12b };

	zassert_equal(
	    alp_testing_adc_queue_raw(channel_id, seq, ARRAY_SIZE(seq)), ALP_OK, "queue_raw failed");

	alp_adc_t *h = open_channel(channel_id);
	zassert_not_null(h, "adc test double must open ANY instance");

	int32_t raw = -1;
	zassert_equal(alp_adc_read_raw(h, &raw), ALP_OK, "read_raw() failed on the 0 boundary");
	zassert_equal(raw, 0, "0 boundary did not round-trip, got %d", raw);

	zassert_equal(alp_adc_read_raw(h, &raw), ALP_OK, "read_raw() failed on the max boundary");
	zassert_equal(raw, max_for_12b, "max-for-resolution boundary did not round-trip, got %d", raw);

	alp_adc_close(h);
}

/* raw -> uV conversion correctness through the dispatcher
 * (src/adc_dispatch.c): the double's documented default open()
 * config is 3.3 V reference / 12-bit resolution
 * (<alp/testing/adc.h>), so the max code (4095) must convert to
 * EXACTLY the reference voltage and 0 must convert to EXACTLY 0 --
 * both boundaries are exact divisions, so this pins the formula
 * (raw * reference_uv / ((1 << resolution_bits) - 1)) without
 * relying on rounding behavior. */
ZTEST(alp_testing_adc_behavior, test_raw_to_uv_conversion_through_dispatcher)
{
	const uint32_t channel_id     = 14;
	const int32_t  max_for_12b    = (1 << 12) - 1;
	const int32_t  default_ref_uv = 3300000; /* documented default, <alp/testing/adc.h> */
	const int32_t  seq[]          = { 0, max_for_12b };

	zassert_equal(
	    alp_testing_adc_queue_raw(channel_id, seq, ARRAY_SIZE(seq)), ALP_OK, "queue_raw failed");

	alp_adc_t *h = open_channel(channel_id);
	zassert_not_null(h, "adc test double must open ANY instance");

	int32_t uv = -1;
	zassert_equal(alp_adc_read_uv(h, &uv), ALP_OK, "read_uv() failed on the 0 boundary");
	zassert_equal(uv, 0, "raw=0 must convert to 0 uV, got %d", uv);

	zassert_equal(alp_adc_read_uv(h, &uv), ALP_OK, "read_uv() failed on the max boundary");
	zassert_equal(uv,
	              default_ref_uv,
	              "raw=max-for-resolution must convert to exactly the reference voltage "
	              "(%d uV), got %d",
	              default_ref_uv,
	              uv);

	alp_adc_close(h);
}

/* fail_next(): the injected status surfaces verbatim, as a documented
 * alp_status_t (not a raw errno), and does not consume the sample it
 * pre-empted. */
ZTEST(alp_testing_adc_behavior, test_fail_next_surfaces_documented_status)
{
	const uint32_t channel_id = 15;
	const int32_t  raw_val    = 100;

	zassert_equal(alp_testing_adc_queue_raw(channel_id, &raw_val, 1), ALP_OK, "queue_raw failed");
	zassert_equal(alp_testing_adc_fail_next(channel_id, ALP_ERR_IO), ALP_OK, "fail_next failed");

	alp_adc_t *h = open_channel(channel_id);
	zassert_not_null(h, "adc test double must open ANY instance");

	int32_t      raw = 0;
	alp_status_t s   = alp_adc_read_raw(h, &raw);
	zassert_equal(s, ALP_ERR_IO, "read_raw() must surface the injected error verbatim");
	zassert_true(status_in_enum(s), "status %d outside alp_status_t", (int)s);

	/* Single-shot and non-consuming: the pre-empted sample is still
	 * there for the next read. */
	zassert_equal(alp_adc_read_raw(h, &raw), ALP_OK, "read_raw() after the fault failed");
	zassert_equal(raw, raw_val, "fail_next() must not consume the sample it pre-empted");

	alp_adc_close(h);
}

/* Reset-frees-side-state regression: queue raw samples and arm a
 * fault without draining/firing either, then reset_all() and prove
 * the channel is fully reusable -- no leftover queue, no stale fault,
 * back to the documented latch-at-0 state. */
ZTEST(alp_testing_adc_behavior, test_reset_all_frees_side_state)
{
	const uint32_t channel_id = 16;
	const int32_t  seq[]      = { 7, 8, 9 };

	zassert_equal(
	    alp_testing_adc_queue_raw(channel_id, seq, ARRAY_SIZE(seq)), ALP_OK, "queue_raw failed");
	zassert_equal(alp_testing_adc_fail_next(channel_id, ALP_ERR_IO), ALP_OK, "fail_next failed");

	alp_adc_t *h = open_channel(channel_id);
	zassert_not_null(h, "adc test double must open ANY instance");
	alp_adc_close(h);

	alp_testing_reset_all();

	/* Channel is reusable: no leftover queue, no leftover fault -- a
	 * fresh open + read returns the documented untouched-latch value. */
	h = open_channel(channel_id);
	zassert_not_null(h, "channel must be reusable after reset_all()");

	int32_t raw = -1;
	zassert_equal(alp_adc_read_raw(h, &raw),
	              ALP_OK,
	              "reset_all() must have cleared the queued fault for this channel");
	zassert_equal(
	    raw, 0, "reset_all() must have cleared the FIFO/latch for this channel, got %d", raw);

	alp_adc_close(h);
}
