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
#include <time.h>
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
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
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

/* Issue #2980 (gpio): alp_gpio_configure() on an input with an IRQ
 * enabled must keep the edge flags, or the kernel stops detecting edges
 * while irq_enabled stays set. */
static void test_configure_input_keeps_edge_flags(void)
{
	reset_fixture();
	int              pipe_fds[2];
	struct alp_gpio *pin = open_fake_pin(pipe_fds);
	ALP_ASSERT_TRUE(pin != NULL);
	ALP_ASSERT_EQ_INT(alp_gpio_irq_enable(pin, ALP_GPIO_EDGE_FALLING, noop_cb, NULL), ALP_OK);
	g_cfg_call_count = 0;

	ALP_ASSERT_EQ_INT(alp_gpio_configure(pin, ALP_GPIO_INPUT, ALP_GPIO_PULL_UP), ALP_OK);
	ALP_ASSERT_EQ_INT(g_cfg_call_count, 1);
	ALP_ASSERT_TRUE((g_cfg_flags[0] & GPIO_V2_LINE_FLAG_EDGE_FALLING) != 0);
	ALP_ASSERT_TRUE((g_cfg_flags[0] & GPIO_V2_LINE_FLAG_BIAS_PULL_UP) != 0);
	ALP_ASSERT_TRUE(pin->irq_enabled);

	alp_gpio_close(pin);
	(void)close(pipe_fds[1]);
}

/* Edge detection cannot coexist with OUTPUT: the IRQ is disabled
 * explicitly and the request carries no edge bits. */
static void test_configure_output_disables_irq(void)
{
	reset_fixture();
	int              pipe_fds[2];
	struct alp_gpio *pin = open_fake_pin(pipe_fds);
	ALP_ASSERT_TRUE(pin != NULL);
	ALP_ASSERT_EQ_INT(alp_gpio_irq_enable(pin, ALP_GPIO_EDGE_BOTH, noop_cb, NULL), ALP_OK);
	g_cfg_call_count = 0;

	ALP_ASSERT_EQ_INT(alp_gpio_configure(pin, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE), ALP_OK);
	ALP_ASSERT_EQ_INT(g_cfg_call_count, 1);
	ALP_ASSERT_EQ_INT(
	    g_cfg_flags[0] & (GPIO_V2_LINE_FLAG_EDGE_RISING | GPIO_V2_LINE_FLAG_EDGE_FALLING), 0);
	ALP_ASSERT_TRUE(!pin->irq_enabled);
	ALP_ASSERT_TRUE(pin->irq_cb == NULL);

	alp_gpio_close(pin);
	(void)close(pipe_fds[1]);
}

/* A slot re-handed out after close gets a new generation, so the
 * dispatcher can tell a reopened pin from the one it polled even when
 * the kernel recycles the same fd number. */
static void test_reacquired_slot_changes_generation(void)
{
	reset_fixture();
	struct alp_gpio *a = pool_acquire();
	ALP_ASSERT_TRUE(a != NULL);
	uint32_t gen_a = a->generation;
	pool_release(a);
	struct alp_gpio *b = pool_acquire();
	ALP_ASSERT_TRUE(b == a);
	ALP_ASSERT_TRUE(b->generation != gen_a);
	pool_release(b);
}

/* ------------------------------------------------------------------ *
 * Dispatcher tests (issue #2980, findings 103/107).
 *
 * These drive the REAL shared dispatcher thread.  To interleave a
 * "poll() said ready" with a concurrent close/reopen or event steal
 * deterministically, the test holds g_irq.mu: the dispatcher's poll()
 * returns, then parks on that same mutex in its per-slot re-validation,
 * and the test changes the slot underneath it before letting go.  If the
 * dispatcher then does a read() that can block, it does so holding
 * g_irq.mu, which the test detects with a bounded timedlock.
 * ------------------------------------------------------------------ */

static void sleep_ms(long ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* true if g_irq.mu can be taken within 1 s (and releases it again). */
static bool irq_mutex_available(void)
{
	struct timespec abs;
	clock_gettime(CLOCK_REALTIME, &abs);
	abs.tv_sec += 1;
	if (pthread_mutex_timedlock(&g_irq.mu, &abs) != 0) {
		return false;
	}
	pthread_mutex_unlock(&g_irq.mu);
	return true;
}

static int g_cb_count;

static void counting_cb(alp_gpio_t *pin, void *user)
{
	(void)pin;
	(void)user;
	__atomic_fetch_add(&g_cb_count, 1, __ATOMIC_SEQ_CST);
}

/* A stale POLLIN for the old owner of a slot must not be acted on by the
 * new owner of that slot, even when the kernel recycled the SAME fd
 * number.  The new owner's fd is deliberately left BLOCKING and empty:
 * if the dispatcher's generation check is missing, it read()s it under
 * g_irq.mu and never returns, so the mutex probe fails. */
static void test_dispatcher_ignores_stale_poll_for_reacquired_slot(void)
{
	g_gpio_test_set_config_hook = fake_set_config;
	__atomic_store_n(&g_cb_count, 0, __ATOMIC_SEQ_CST);

	int              a[2], b[2];
	struct alp_gpio *pin = open_fake_pin(a);
	ALP_ASSERT_TRUE(pin != NULL);
	ALP_ASSERT_EQ_INT(pipe(b), 0);
	ALP_ASSERT_EQ_INT(gpio_set_nonblock(a[0]), 0);
	ALP_ASSERT_EQ_INT(alp_gpio_irq_enable(pin, ALP_GPIO_EDGE_BOTH, counting_cb, NULL), ALP_OK);
	sleep_ms(100); /* dispatcher has snapshotted the pin and is in poll() */

	pthread_mutex_lock(&g_irq.mu);
	uint8_t byte = 1;
	ALP_ASSERT_EQ_INT(write(a[1], &byte, 1), 1); /* poll() returns POLLIN for the old owner */
	sleep_ms(100);                               /* dispatcher is now parked on g_irq.mu */
	/* Emulate close + reopen onto the same slot and fd number: the new
	 * owner's fd replaces the old one atomically, and pool_acquire()
	 * bumps the generation. */
	int fdnum = pin->line_fd;
	ALP_ASSERT_TRUE(dup2(b[0], fdnum) == fdnum);
	(void)close(b[0]);
	pin->generation += 1u;
	pthread_mutex_unlock(&g_irq.mu);
	sleep_ms(100); /* dispatcher takes the mutex and runs its re-validation */

	bool available = irq_mutex_available();
	ALP_ASSERT_TRUE(available);
	ALP_ASSERT_EQ_INT(__atomic_load_n(&g_cb_count, __ATOMIC_SEQ_CST), 0);

	if (!available) {
		/* Unblock a dispatcher stuck in read() so the process can finish. */
		(void)write(b[1], &byte, 1);
		sleep_ms(100);
	}
	alp_gpio_close(pin);
	(void)close(a[1]);
	(void)close(b[1]);
}

/* A level that poll() reported can be gone by the time the dispatcher
 * reads it (another reader drained it).  alp_gpio_open() makes the line
 * fd O_NONBLOCK so that read() returns EAGAIN instead of parking the
 * dispatcher -- and every later g_irq.mu taker -- until the next edge.
 * The test sets the flag through the same helper alp_gpio_open() uses. */
static void test_dispatcher_read_does_not_park_under_mutex(void)
{
	g_gpio_test_set_config_hook = fake_set_config;
	__atomic_store_n(&g_cb_count, 0, __ATOMIC_SEQ_CST);

	int              a[2];
	struct alp_gpio *pin = open_fake_pin(a);
	ALP_ASSERT_TRUE(pin != NULL);
	ALP_ASSERT_EQ_INT(gpio_set_nonblock(a[0]), 0);
	ALP_ASSERT_EQ_INT(alp_gpio_irq_enable(pin, ALP_GPIO_EDGE_BOTH, counting_cb, NULL), ALP_OK);
	sleep_ms(100);

	pthread_mutex_lock(&g_irq.mu);
	uint8_t byte = 1;
	ALP_ASSERT_EQ_INT(write(a[1], &byte, 1), 1);
	sleep_ms(100); /* dispatcher: poll() returned, parked on g_irq.mu */
	ALP_ASSERT_EQ_INT(read(a[0], &byte, 1), 1); /* the event is stolen before its read() */
	pthread_mutex_unlock(&g_irq.mu);
	sleep_ms(100); /* dispatcher takes the mutex and read()s the now-empty line */

	bool available = irq_mutex_available();
	ALP_ASSERT_TRUE(available);
	ALP_ASSERT_EQ_INT(__atomic_load_n(&g_cb_count, __ATOMIC_SEQ_CST), 0);

	if (!available) {
		(void)write(a[1], &byte, 1);
		sleep_ms(100);
	}
	alp_gpio_close(pin);
	(void)close(a[1]);
}

/* Close must wait for an op already past its lifecycle check instead of
 * closing line_fd under it (issue #2980).  The values-ioctl hook blocks
 * on the pipe the handle's line_fd points at, so the alp_gpio_write() is
 * genuinely in flight (and holds no g_irq.mu) while close() runs. */
static int blocking_values_ioctl(int line_fd, unsigned long req, void *vals)
{
	(void)req;
	(void)vals;
	uint8_t b;
	return (read(line_fd, &b, 1) == 1) ? 0 : -1;
}

static void *write_thread(void *arg)
{
	(void)alp_gpio_write(arg, true);
	return NULL;
}

static void *close_thread(void *arg)
{
	alp_gpio_close(arg);
	return NULL;
}

static void test_close_waits_for_in_flight_op(void)
{
	g_gpio_test_values_hook = blocking_values_ioctl;
	int              a[2];
	struct alp_gpio *pin = open_fake_pin(a);
	ALP_ASSERT_TRUE(pin != NULL);
	pin->is_output = true;
	int fd         = pin->line_fd;

	pthread_t op, cl;
	ALP_ASSERT_EQ_INT(pthread_create(&op, NULL, write_thread, pin), 0);
	while (__atomic_load_n(&pin->active_ops, __ATOMIC_ACQUIRE) == 0u) {
		sleep_ms(1);
	}
	ALP_ASSERT_EQ_INT(pthread_create(&cl, NULL, close_thread, pin), 0);
	while (alp_lifecycle_get(&pin->lifecycle) == ALP_HANDLE_LC_OPEN) {
		sleep_ms(1);
	}
	sleep_ms(20);
	ALP_ASSERT_TRUE(pin->line_fd == fd); /* still open: close is draining */
	ALP_ASSERT_EQ_INT(alp_gpio_write(pin, false), ALP_ERR_NOT_READY);

	uint8_t byte = 1;
	ALP_ASSERT_EQ_INT(write(a[1], &byte, 1), 1);
	pthread_join(op, NULL);
	pthread_join(cl, NULL);
	ALP_ASSERT_TRUE(!pin->in_use);
	(void)close(a[1]);
	g_gpio_test_values_hook = NULL;
}

static int failing_set_config(int line_fd, const struct gpio_v2_line_config *cfg)
{
	(void)line_fd;
	(void)cfg;
	errno = EIO;
	return -1;
}

/* A failed SET_CONFIG must leave the software IRQ state matching the
 * (unchanged) kernel line: the IRQ stays enabled. */
static void test_configure_output_failure_keeps_irq(void)
{
	reset_fixture();
	int              a[2];
	struct alp_gpio *pin = open_fake_pin(a);
	ALP_ASSERT_TRUE(pin != NULL);
	ALP_ASSERT_EQ_INT(alp_gpio_irq_enable(pin, ALP_GPIO_EDGE_BOTH, noop_cb, NULL), ALP_OK);

	g_gpio_test_set_config_hook = failing_set_config;
	ALP_ASSERT_TRUE(alp_gpio_configure(pin, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE) != ALP_OK);
	ALP_ASSERT_TRUE(pin->irq_enabled);
	ALP_ASSERT_TRUE(pin->irq_cb == noop_cb);
	ALP_ASSERT_TRUE(!pin->is_output);

	g_gpio_test_set_config_hook = fake_set_config;
	alp_gpio_close(pin);
	(void)close(a[1]);
}

int main(void)
{
	/* Dispatcher tests first: they rely on one live dispatcher, and the
	 * rollback tests below deliberately restart it via g_irq.started. */
	test_dispatcher_ignores_stale_poll_for_reacquired_slot();
	test_dispatcher_read_does_not_park_under_mutex();
	test_close_waits_for_in_flight_op();
	test_configure_output_failure_keeps_irq();
	test_eventfd_failure_rolls_back_to_plain_input();
	test_pthread_create_failure_rolls_back_to_plain_input();
	test_success_path_enables_irq_without_rollback();
	test_configure_input_keeps_edge_flags();
	test_configure_output_disables_irq();
	test_reacquired_slot_changes_generation();

	ALP_TEST_SUMMARY();
}
