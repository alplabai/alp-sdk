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

ZTEST_SUITE(alp_testing_gpio_behavior, NULL, NULL, gpio_behavior_before, NULL, NULL);

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
