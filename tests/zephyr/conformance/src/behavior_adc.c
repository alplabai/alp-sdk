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

/* #757: alp_adc_read_uv must reject an out-of-range backend-resolved
 * resolution_bits instead of shifting `1` by it -- resolution_bits=32
 * on a 32-bit `1u` is undefined behavior (src/adc_dispatch.c), and
 * under CONFIG_ALP_SOC_NONE (this build) the open()-time SoC-cap gate
 * is compiled out (ALP_SOC_ADC_MAX_RESOLUTION_BITS == UINT16_MAX), so
 * nothing upstream of alp_adc_read_uv catches it either -- the test
 * double happily stores whatever resolution_bits the caller passed.
 *
 * NON-DISCRIMINATING BOUNDARY CHECK, not a regression test: on the
 * x86-64 host toolchain this suite actually builds with (plain
 * twister, no UBSan), `1u << 32` lowers to a hardware SHL that masks
 * the count mod 32, so the PRE-FIX code's `(1u << 32) - 1u` also
 * evaluates to 0 and ALSO returns ALP_ERR_NOT_READY here -- by
 * coincidence, not because the old code validated anything. This case
 * is kept only to pin exactly the value #757 names; it does NOT prove
 * the fix. See test_read_uv_rejects_resolution_bits_over_31_regression
 * below for the case that actually discriminates old vs. new code
 * under this same (non-UBSan) gate. */
ZTEST(alp_testing_adc_behavior, test_read_uv_rejects_resolution_bits_32)
{
	const uint32_t channel_id = 17;
	const int32_t  raw_val    = 1;

	zassert_equal(alp_testing_adc_queue_raw(channel_id, &raw_val, 1), ALP_OK, "queue_raw failed");

	alp_adc_config_t cfg = ALP_ADC_CONFIG_DEFAULT(channel_id);
	cfg.resolution_bits  = 32u;
	alp_adc_t *h         = alp_adc_open(&cfg);
	zassert_not_null(h, "adc test double must open ANY instance regardless of resolution_bits");

	int32_t      uv = -1;
	alp_status_t s  = alp_adc_read_uv(h, &uv);
	zassert_equal(s,
	              ALP_ERR_NOT_READY,
	              "resolution_bits=32 must be rejected before the width-unsafe shift, got %d",
	              (int)s);
	zassert_true(status_in_enum(s), "status %d outside alp_status_t", (int)s);

	alp_adc_close(h);
}

/* Actual regression test for #757, provable under the plain (no
 * UBSan) twister gate CI runs: resolution_bits=40 is >31 in both the
 * pre-fix and post-fix code, but the pre-fix shift-mod-32 hardware
 * behavior gives it a DIFFERENT (non-zero) full-scale than
 * resolution_bits=32 does -- `(1u << 40) - 1u` lowers to `(1u << 8) -
 * 1u` == 255 on this host toolchain, so the pre-fix code does NOT
 * bail out: it silently computes a bogus converted voltage and
 * returns ALP_OK. The post-fix explicit `> 31` guard rejects it
 * regardless of the shift's hardware behavior. This is the case that
 * fails (wrongly returns ALP_OK) if src/adc_dispatch.c's `> 31` guard
 * is removed -- see the mutation-proof transcript in the PR/commit
 * description. */
ZTEST(alp_testing_adc_behavior, test_read_uv_rejects_resolution_bits_over_31_regression)
{
	const uint32_t channel_id = 19;
	const int32_t  raw_val    = 1;

	zassert_equal(alp_testing_adc_queue_raw(channel_id, &raw_val, 1), ALP_OK, "queue_raw failed");

	alp_adc_config_t cfg = ALP_ADC_CONFIG_DEFAULT(channel_id);
	cfg.resolution_bits  = 40u;
	alp_adc_t *h         = alp_adc_open(&cfg);
	zassert_not_null(h, "adc test double must open ANY instance regardless of resolution_bits");

	int32_t      uv = -1;
	alp_status_t s  = alp_adc_read_uv(h, &uv);
	zassert_equal(s,
	              ALP_ERR_NOT_READY,
	              "resolution_bits=40 must be rejected (>31), got %d (uv=%d)",
	              (int)s,
	              uv);
	zassert_true(status_in_enum(s), "status %d outside alp_status_t", (int)s);

	alp_adc_close(h);
}

/* The adjacent in-range boundary (31 bits) must still convert
 * correctly -- pins the guard at exactly ">31", not an
 * off-by-one that also rejects the last valid width. */
ZTEST(alp_testing_adc_behavior, test_read_uv_accepts_resolution_bits_31)
{
	const uint32_t channel_id = 18;
	const int32_t  raw_val    = 0;

	zassert_equal(alp_testing_adc_queue_raw(channel_id, &raw_val, 1), ALP_OK, "queue_raw failed");

	alp_adc_config_t cfg = ALP_ADC_CONFIG_DEFAULT(channel_id);
	cfg.resolution_bits  = 31u;
	alp_adc_t *h         = alp_adc_open(&cfg);
	zassert_not_null(h);

	int32_t      uv = -1;
	alp_status_t s  = alp_adc_read_uv(h, &uv);
	zassert_equal(s, ALP_OK, "resolution_bits=31 must be accepted, got %d", (int)s);
	zassert_equal(uv, 0, "raw=0 must convert to 0 uV regardless of resolution width");

	alp_adc_close(h);
}

/* #749: the alp_last_error() contract (<alp/peripheral.h>) says every
 * successful alp_*_open clears the thread-local slot -- alp_adc_open
 * was missing the alp_z_clear_last_error() call the sibling i2c/spi
 * dispatchers make at open() entry, so a prior failure's error code
 * kept reading back after a LATER successful open. */
ZTEST(alp_testing_adc_behavior, test_open_success_clears_stale_last_error)
{
	/* Provoke a failure first so the thread-local slot holds a
	 * non-OK value. */
	zassert_is_null(alp_adc_open(NULL));
	zassert_equal(alp_last_error(), ALP_ERR_INVAL, "setup: NULL cfg must set last-error");

	alp_adc_t *h = open_channel(19);
	zassert_not_null(h, "adc test double must open ANY instance");
	zassert_equal(alp_last_error(),
	              ALP_OK,
	              "a successful open() must clear the stale error from the earlier failed call");

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
