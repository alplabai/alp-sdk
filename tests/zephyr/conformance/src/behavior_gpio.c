/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Behavioral suite for the alp/testing GPIO virtual backend
 * (epic #610 PR1).  Compiled only for the alp_sdk.conformance.
 * test_doubles twister scenario (CONFIG_ALP_SDK_TESTING=y) -- see
 * testcase.yaml.  Drives the double through the PUBLIC alp/peripheral.h
 * API plus the alp/testing injection surface; never touches
 * gpio_ops.h / testing_drv.c internals directly.
 *
 * This scenario's CMakeLists swaps this file in for src/main.c
 * (untouched) instead of adding it alongside: main.c's per-class rows
 * assume the real/emulated backend (e.g. gpio's Case B expects
 * alp_gpio_open(99) to FAIL, which no longer holds once a priority-255
 * "open ANY instance" test double is selected for the class), so the
 * two suites are built as separate app images, never linked into the
 * same binary.  The setup fixture below still fails loudly (rather
 * than silently exercising the wrong backend) if selection ever picks
 * anything other than the priority-255 test double.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>
#include <alp/testing/clock.h>
#include <alp/testing/common.h>
#include <alp/testing/gpio.h>

/* Local copy of main.c's enum-membership helper -- main.c's is static
 * to that TU, and this file's "error translation" case needs the same
 * check without touching main.c. */
static bool status_in_enum(alp_status_t s)
{
	return s <= ALP_OK && s >= ALP_STATUS_ENUM_FLOOR;
}

static int   g_cb_count;
static void *g_cb_last_pin;
static void *g_cb_last_user;

static void test_cb(alp_gpio_t *pin, void *user)
{
	g_cb_count++;
	g_cb_last_pin  = pin;
	g_cb_last_user = user;
}

static void gpio_behavior_before(void *fixture)
{
	ARG_UNUSED(fixture);
	alp_testing_reset_all();
	g_cb_count     = 0;
	g_cb_last_pin  = NULL;
	g_cb_last_user = NULL;
}

/* After-each teardown (#610 review, Fix 4): alp_testing_reset_all()
 * wipes every alp/testing double's own state, but it cannot reach the
 * DISPATCHER's private static handle pool (CONFIG_ALP_SDK_MAX_GPIO_HANDLES
 * slots, src/gpio_dispatch.c) -- that pool is portable-API state, not
 * a testing double's.  A test in this file that leaks a handle (opens
 * one and never closes it) would otherwise silently shrink the pool
 * for every later test until it quietly runs out, surfacing as a
 * confusing ALP_ERR_NOMEM far from the actual leak.  Round-tripping a
 * fresh handle here, right after the test that may have leaked one,
 * fails loudly and immediately instead. */
static void gpio_behavior_after(void *fixture)
{
	ARG_UNUSED(fixture);
	alp_testing_reset_all();

	alp_gpio_t *h = alp_gpio_open(0);
	zassert_not_null(h,
	                 "pool-health check failed: alp_gpio_open(0) returned NULL right after "
	                 "this test -- a prior test in this file leaked a handle out of the "
	                 "dispatcher's fixed-size pool");
	alp_gpio_close(h);
}

ZTEST_SUITE(alp_testing_gpio_behavior, NULL, NULL, gpio_behavior_before, gpio_behavior_after, NULL);

/* Setup-fixture-shaped assertion (issue #610): a mis-selection (e.g. a
 * higher-priority real/proxy backend somehow beating the test double)
 * must fail LOUDLY, not silently exercise the wrong backend for every
 * other case below. */
ZTEST(alp_testing_gpio_behavior, test_backend_selection_is_the_test_double)
{
	const alp_backend_t *be = alp_backend_select("gpio", ALP_SOC_REF_STR);

	zassert_not_null(be, "gpio class has no registered backend at all");
	zassert_equal(be->priority,
	              255,
	              "gpio backend selection picked priority %u, not the "
	              "reserved test-double priority 255 -- CONFIG_ALP_SDK_TESTING_GPIO "
	              "not set, or a higher-priority backend was added",
	              (unsigned)be->priority);
	zassert_equal(strcmp(be->vendor, "alp_testing"),
	              0,
	              "gpio backend selection picked vendor '%s', not 'alp_testing'",
	              be->vendor);
}

ZTEST(alp_testing_gpio_behavior, test_input_read_back)
{
	const uint32_t pin_id = 10;

	zassert_equal(alp_testing_gpio_set_input(pin_id, true), ALP_OK, "set_input(true) failed");

	alp_gpio_t *h = alp_gpio_open(pin_id);
	zassert_not_null(h, "gpio test double must open ANY instance");

	bool level = false;
	zassert_equal(alp_gpio_read(h, &level), ALP_OK, "read() failed");
	zassert_true(level, "read() did not observe the injected high level");

	zassert_equal(alp_testing_gpio_set_input(pin_id, false), ALP_OK, "set_input(false) failed");
	zassert_equal(alp_gpio_read(h, &level), ALP_OK, "read() failed");
	zassert_false(level, "read() did not observe the injected low level");

	alp_gpio_close(h);
}

ZTEST(alp_testing_gpio_behavior, test_output_write_and_count)
{
	const uint32_t pin_id = 11;

	alp_gpio_t *h = alp_gpio_open(pin_id);
	zassert_not_null(h, "gpio test double must open ANY instance");

	zassert_equal(
	    alp_gpio_configure(h, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE), ALP_OK, "configure() failed");

	bool     level;
	uint32_t n;

	zassert_equal(alp_gpio_write(h, true), ALP_OK, "write(true) failed");
	zassert_equal(alp_testing_gpio_get_output(pin_id, &level), ALP_OK, "get_output failed");
	zassert_true(level, "get_output did not observe the driven high level");

	zassert_equal(alp_gpio_write(h, false), ALP_OK, "write(false) failed");
	zassert_equal(alp_testing_gpio_get_output(pin_id, &level), ALP_OK, "get_output failed");
	zassert_false(level, "get_output did not observe the driven low level");

	zassert_equal(alp_testing_gpio_write_count(pin_id, &n), ALP_OK, "write_count failed");
	zassert_equal(n, 2, "expected 2 alp_gpio_write() calls, counted %u", n);

	alp_gpio_close(h);
}

ZTEST(alp_testing_gpio_behavior, test_edge_fires_cb_on_matching_arm)
{
	const uint32_t pin_id = 12;

	alp_gpio_t *h = alp_gpio_open(pin_id);
	zassert_not_null(h, "gpio test double must open ANY instance");
	zassert_equal(alp_gpio_irq_enable(h, ALP_GPIO_EDGE_RISING, test_cb, (void *)0x1234),
	              ALP_OK,
	              "irq_enable(RISING) failed");

	zassert_equal(
	    alp_testing_gpio_edge(pin_id, ALP_GPIO_EDGE_RISING), ALP_OK, "edge injection failed");

	zassert_equal(g_cb_count, 1, "cb did not fire on a matching armed edge");
	zassert_equal(g_cb_last_pin, h, "cb fired with the wrong pin handle");
	zassert_equal(g_cb_last_user, (void *)0x1234, "cb fired with the wrong user pointer");

	alp_gpio_close(h);
}

ZTEST(alp_testing_gpio_behavior, test_edge_does_not_fire_cb_on_mismatched_arm)
{
	const uint32_t pin_id = 13;

	alp_gpio_t *h = alp_gpio_open(pin_id);
	zassert_not_null(h, "gpio test double must open ANY instance");
	zassert_equal(alp_gpio_irq_enable(h, ALP_GPIO_EDGE_FALLING, test_cb, NULL),
	              ALP_OK,
	              "irq_enable(FALLING) failed");

	zassert_equal(
	    alp_testing_gpio_edge(pin_id, ALP_GPIO_EDGE_RISING), ALP_OK, "edge injection failed");

	zassert_equal(g_cb_count, 0, "cb fired despite the injected edge not matching the armed one");

	alp_gpio_close(h);
}

ZTEST(alp_testing_gpio_behavior, test_edge_at_deferred_via_clock_advance)
{
	const uint32_t pin_id = 14;

	alp_gpio_t *h = alp_gpio_open(pin_id);
	zassert_not_null(h, "gpio test double must open ANY instance");
	zassert_equal(alp_gpio_irq_enable(h, ALP_GPIO_EDGE_RISING, test_cb, NULL),
	              ALP_OK,
	              "irq_enable(RISING) failed");

	zassert_equal(
	    alp_testing_gpio_edge_at(pin_id, 100, ALP_GPIO_EDGE_RISING), ALP_OK, "edge_at() failed");
	zassert_equal(g_cb_count, 0, "edge_at() must not fire immediately");

	zassert_equal(alp_testing_clock_advance_ms(50), ALP_OK, "advance_ms(50) failed");
	zassert_equal(g_cb_count, 0, "cb fired before the scheduled timestamp");

	zassert_equal(alp_testing_clock_advance_ms(60), ALP_OK, "advance_ms(60) failed");
	zassert_equal(g_cb_count, 1, "cb did not fire once the virtual clock passed at_ms");
	zassert_true(alp_testing_clock_now_ms() >= 110, "virtual clock did not advance");

	alp_gpio_close(h);
}

ZTEST(alp_testing_gpio_behavior, test_arm_then_close_then_inject_is_a_no_op)
{
	const uint32_t pin_id = 15;

	alp_gpio_t *h = alp_gpio_open(pin_id);
	zassert_not_null(h, "gpio test double must open ANY instance");
	zassert_equal(alp_gpio_irq_enable(h, ALP_GPIO_EDGE_RISING, test_cb, NULL),
	              ALP_OK,
	              "irq_enable(RISING) failed");

	alp_gpio_close(h);

	zassert_equal(alp_testing_gpio_edge(pin_id, ALP_GPIO_EDGE_RISING),
	              ALP_OK,
	              "edge injection on a closed pin id must not itself error");
	zassert_equal(
	    g_cb_count, 0, "cb fired after close() -- use-after-close into a freed/reused handle");
}

ZTEST(alp_testing_gpio_behavior, test_capability_op_consistency)
{
	const uint32_t pin_id = 16;

	alp_gpio_t *h = alp_gpio_open(pin_id);
	zassert_not_null(h, "gpio test double must open ANY instance");

	const alp_capabilities_t *caps = alp_gpio_capabilities(h);
	zassert_not_null(caps, "capabilities() must be non-NULL for an open handle");

	/* Every op the ops table wires must actually work -- a class
	 * double that advertised (or silently implied) a capability its
	 * ops struct then refused with NOSUPPORT would be a caps<->ops
	 * inconsistency the conformance gate exists to catch. */
	zassert_equal(alp_gpio_configure(h, ALP_GPIO_INPUT, ALP_GPIO_PULL_NONE),
	              ALP_OK,
	              "configure() must not be NOSUPPORT on this double");
	bool level;
	zassert_equal(alp_gpio_read(h, &level), ALP_OK, "read() must not be NOSUPPORT on this double");
	zassert_equal(alp_gpio_irq_enable(h, ALP_GPIO_EDGE_BOTH, test_cb, NULL),
	              ALP_OK,
	              "enable_irq() must not be NOSUPPORT on this double");
	zassert_equal(
	    alp_gpio_irq_disable(h), ALP_OK, "disable_irq() must not be NOSUPPORT on this double");

	alp_gpio_close(h);
}

ZTEST(alp_testing_gpio_behavior, test_error_translation_is_in_enum)
{
	/* get_output()/write_count() document ALP_ERR_INVAL for a pin id
	 * that has never been opened or injected -- pick one nothing in
	 * this suite (reset between cases) has touched. */
	const uint32_t untouched_pin = 0xFEED;
	bool           level;
	uint32_t       n;

	alp_status_t s1 = alp_testing_gpio_get_output(untouched_pin, &level);
	zassert_equal(s1, ALP_ERR_INVAL, "get_output(never-touched id) must be ALP_ERR_INVAL");
	zassert_true(status_in_enum(s1), "status %d outside alp_status_t", (int)s1);

	alp_status_t s2 = alp_testing_gpio_write_count(untouched_pin, &n);
	zassert_equal(s2, ALP_ERR_INVAL, "write_count(never-touched id) must be ALP_ERR_INVAL");
	zassert_true(status_in_enum(s2), "status %d outside alp_status_t", (int)s2);

	alp_status_t s3 = alp_testing_gpio_edge(untouched_pin + 1, ALP_GPIO_EDGE_NONE);
	zassert_equal(s3, ALP_ERR_INVAL, "edge(EDGE_NONE) must document ALP_ERR_INVAL");
	zassert_true(status_in_enum(s3), "status %d outside alp_status_t", (int)s3);
}

/* #610 review, Fix 1 (MAJOR): reset_all() must free every deferred
 * edge that was scheduled but never fired (advance_ms() never reached
 * it) -- not just the clock's own event queue and the instance
 * tables.  Without the reset-hook registry (src/testing/
 * reset_registry.h) that clears g_deferred in testing_drv.c, this
 * pool (capacity ALP_TESTING_GPIO_MAX_DEFERRED == 8) leaks one slot
 * per unfired edge_at() across every alp_testing_reset_all(), and the
 * 9th edge_at() ever scheduled in the whole test run -- regardless of
 * which test schedules it -- fails ALP_ERR_NOMEM. */
ZTEST(alp_testing_gpio_behavior, test_reset_all_frees_unfired_deferred_edges)
{
	const uint32_t pin_id = 20;

	/* Fill the deferred-edge pool without ever advancing the clock, so
	 * nothing fires and nothing frees itself the normal way. */
	for (uint32_t i = 0; i < 8; ++i) {
		zassert_equal(alp_testing_gpio_edge_at(pin_id, 100 + i, ALP_GPIO_EDGE_RISING),
		              ALP_OK,
		              "edge_at() #%u failed while filling the deferred pool",
		              i);
	}
	/* Sanity: prove the pool really is exhausted before the reset --
	 * otherwise the leak this test guards against could go unnoticed. */
	zassert_equal(alp_testing_gpio_edge_at(pin_id, 200, ALP_GPIO_EDGE_RISING),
	              ALP_ERR_NOMEM,
	              "deferred-edge pool should already be full before reset_all()");

	alp_testing_reset_all();

	/* This is the fix under test: every slot must be free again, so
	 * refilling the pool from scratch succeeds start to finish. */
	for (uint32_t i = 0; i < 8; ++i) {
		zassert_equal(alp_testing_gpio_edge_at(pin_id, 100 + i, ALP_GPIO_EDGE_RISING),
		              ALP_OK,
		              "edge_at() #%u failed after reset_all() -- deferred-edge pool leaked",
		              i);
	}
}

/* #610 review, Fix 5(a): the synchronous-fire contract
 * (<alp/testing/gpio.h>'s alp_testing_gpio_edge_at doc) means a cb
 * runs ON the injecting call's own thread/stack, so it must be safe
 * for that cb to call back INTO the portable API on its own handle --
 * this pins that a read from inside the cb observes the same edge
 * that just fired it, with no reentrancy corruption. */
static bool         g_cb_reentrant_read_level;
static alp_status_t g_cb_reentrant_read_rc = ALP_ERR_INVAL;

static void cb_reads_own_handle(alp_gpio_t *pin, void *user)
{
	ARG_UNUSED(user);
	g_cb_count++;
	g_cb_reentrant_read_rc = alp_gpio_read(pin, &g_cb_reentrant_read_level);
}

ZTEST(alp_testing_gpio_behavior, test_cb_reentrant_read_is_safe)
{
	const uint32_t pin_id = 21;

	alp_gpio_t *h = alp_gpio_open(pin_id);
	zassert_not_null(h, "gpio test double must open ANY instance");
	zassert_equal(alp_gpio_irq_enable(h, ALP_GPIO_EDGE_RISING, cb_reads_own_handle, NULL),
	              ALP_OK,
	              "irq_enable(RISING) failed");

	zassert_equal(
	    alp_testing_gpio_edge(pin_id, ALP_GPIO_EDGE_RISING), ALP_OK, "edge injection failed");

	zassert_equal(g_cb_count, 1, "cb did not fire");
	zassert_equal(
	    g_cb_reentrant_read_rc, ALP_OK, "alp_gpio_read() called from inside the cb failed");
	zassert_true(g_cb_reentrant_read_level,
	             "read() from inside the cb did not observe the edge that fired it");

	alp_gpio_close(h);
}

/* #610 review, Fix 5(b): a cb that closes its OWN handle while still
 * being invoked from inside the injection that fired it must not
 * crash or use-after-free -- fire_edge() (testing_drv.c) makes no
 * slot access after calling the cb, so this is safe by construction,
 * but that invariant is exactly the kind of thing a future double
 * copying this pattern could break silently.  Pin it here. */
static void cb_closes_own_handle(alp_gpio_t *pin, void *user)
{
	ARG_UNUSED(user);
	g_cb_count++;
	alp_gpio_close(pin);
}

ZTEST(alp_testing_gpio_behavior, test_cb_reentrant_close_is_safe)
{
	const uint32_t pin_id = 22;

	alp_gpio_t *h = alp_gpio_open(pin_id);
	zassert_not_null(h, "gpio test double must open ANY instance");
	zassert_equal(alp_gpio_irq_enable(h, ALP_GPIO_EDGE_RISING, cb_closes_own_handle, NULL),
	              ALP_OK,
	              "irq_enable(RISING) failed");

	zassert_equal(alp_testing_gpio_edge(pin_id, ALP_GPIO_EDGE_RISING),
	              ALP_OK,
	              "edge injection must not itself fail because the cb closed its own handle");
	zassert_equal(g_cb_count, 1, "cb did not fire");

	/* h was already closed by the cb; a second close() must stay the
	 * documented no-op (idempotent void close), not a double-free. */
	alp_gpio_close(h);
}
