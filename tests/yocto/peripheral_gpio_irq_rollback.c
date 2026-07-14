/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #746: alp_gpio_irq_enable()
 * (src/yocto/peripheral_gpio.c) reconfigures the line for edge
 * detection before starting the shared IRQ dispatcher.  If the
 * dispatcher fails to start (eventfd() or pthread_create() failure),
 * the line must be rolled back to plain-input rather than left
 * edge-configured while irq_enabled stays false.
 *
 * This file #includes the real backend .c file directly (same
 * technique as tests/yocto/rpc_yocto_self_close.c) to reach its
 * file-local `struct alp_gpio` / pool_acquire() / pool_release() and
 * the two test seams it exposes: g_gpio_test_set_config_hook (every
 * GPIO_V2_LINE_SET_CONFIG_IOCTL this backend issues) and
 * g_gpio_test_fail_dispatcher_start (forces the eventfd()- or
 * pthread_create()-failure branch inside irq_start_dispatcher_locked()
 * deterministically).  The handle's line_fd is a pipe read-end -- safe
 * to open/close, never touched by a real ioctl since the SET_CONFIG
 * seam intercepts every call this test drives.  This deliberately
 * avoids the real /dev/gpiochipN nodes present on a dev host: those
 * back genuine (non-alp-sdk-target) ACPI/EC GPIO lines a test must
 * never reconfigure.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_gpio_irq_rollback
 *   ctest --test-dir build -R alp_test_peripheral_gpio_irq_rollback
 */

#include <stdint.h>
#include <unistd.h>

#include "test_assert.h"

#include "../../src/yocto/peripheral_gpio.c"

#define MAX_CFG_CALLS 8

static uint64_t g_cfg_flags[MAX_CFG_CALLS];
static int      g_cfg_call_count;

static int fake_set_config(int line_fd, const struct gpio_v2_line_config *cfg)
{
	(void)line_fd;
	if (g_cfg_call_count < MAX_CFG_CALLS) {
		g_cfg_flags[g_cfg_call_count] = cfg->flags;
	}
	++g_cfg_call_count;
	return 0; /* always "succeeds" against the fake line */
}

static void noop_cb(alp_gpio_t *pin, void *user)
{
	(void)pin;
	(void)user;
}

static struct alp_gpio *open_fake_pin(int *pipe_fds)
{
	if (pipe(pipe_fds) != 0) {
		ALP_TEST_FAIL("pipe() failed unexpectedly");
		return NULL;
	}
	struct alp_gpio *h = pool_acquire();
	if (h == NULL) {
		ALP_TEST_FAIL("pool_acquire() returned NULL");
		return NULL;
	}
	h->line_fd = pipe_fds[0];
	return h;
}

static void reset_fixture(void)
{
	g_cfg_call_count                  = 0;
	g_gpio_test_set_config_hook       = fake_set_config;
	g_gpio_test_fail_dispatcher_start = 0;
	/* Force irq_start_dispatcher_locked() to actually re-run its start
	 * sequence for the next call instead of short-circuiting on an
	 * already-started dispatcher from a previous test. */
	g_irq.started = false;
}

/* ------------------------------------------------------------------ */

static void test_eventfd_failure_rolls_back_to_plain_input(void)
{
	reset_fixture();
	g_gpio_test_fail_dispatcher_start = 1; /* force the eventfd() failure point */

	int              pipe_fds[2];
	struct alp_gpio *pin = open_fake_pin(pipe_fds);
	ALP_ASSERT_TRUE(pin != NULL);

	alp_status_t rc = alp_gpio_irq_enable(pin, ALP_GPIO_EDGE_RISING, noop_cb, NULL);
	ALP_ASSERT_TRUE(rc != ALP_OK);
	ALP_ASSERT_EQ_INT(g_cfg_call_count, 2); /* edge-configure, then rollback */
	ALP_ASSERT_TRUE((g_cfg_flags[0] & GPIO_V2_LINE_FLAG_EDGE_RISING) != 0);
	ALP_ASSERT_EQ_INT(g_cfg_flags[1], GPIO_V2_LINE_FLAG_INPUT);
	ALP_ASSERT_TRUE(!pin->irq_enabled);

	alp_gpio_close(pin);
	(void)close(pipe_fds[1]);
}

static void test_pthread_create_failure_rolls_back_to_plain_input(void)
{
	reset_fixture();
	g_gpio_test_fail_dispatcher_start = 2; /* force the pthread_create() failure point */

	int              pipe_fds[2];
	struct alp_gpio *pin = open_fake_pin(pipe_fds);
	ALP_ASSERT_TRUE(pin != NULL);

	alp_status_t rc = alp_gpio_irq_enable(pin, ALP_GPIO_EDGE_FALLING, noop_cb, NULL);
	ALP_ASSERT_TRUE(rc != ALP_OK);
	ALP_ASSERT_EQ_INT(g_cfg_call_count, 2); /* edge-configure, then rollback */
	ALP_ASSERT_TRUE((g_cfg_flags[0] & GPIO_V2_LINE_FLAG_EDGE_FALLING) != 0);
	ALP_ASSERT_EQ_INT(g_cfg_flags[1], GPIO_V2_LINE_FLAG_INPUT);
	ALP_ASSERT_TRUE(!pin->irq_enabled);

	alp_gpio_close(pin);
	(void)close(pipe_fds[1]);
}

static void test_success_path_enables_irq_without_rollback(void)
{
	reset_fixture();
	/* g_gpio_test_fail_dispatcher_start stays 0: real eventfd() +
	 * pthread_create() run and the dispatcher genuinely starts, same
	 * as production -- proving the fix does not roll back on success. */
	int              pipe_fds[2];
	struct alp_gpio *pin = open_fake_pin(pipe_fds);
	ALP_ASSERT_TRUE(pin != NULL);

	alp_status_t rc = alp_gpio_irq_enable(pin, ALP_GPIO_EDGE_BOTH, noop_cb, NULL);
	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_TRUE(pin->irq_enabled);
	ALP_ASSERT_EQ_INT(g_cfg_call_count, 1); /* only the edge-configure; no rollback */

	alp_gpio_close(pin);
	(void)close(pipe_fds[1]);
}

int main(void)
{
	test_eventfd_failure_rolls_back_to_plain_input();
	test_pthread_create_failure_rolls_back_to_plain_input();
	test_success_path_enables_irq_without_rollback();

	ALP_TEST_SUMMARY();
}
