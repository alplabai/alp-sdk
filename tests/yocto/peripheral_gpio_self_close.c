/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #756: the Yocto direct-GPIO IRQ
 * dispatcher (src/yocto/peripheral_gpio.c) used to invoke a pin's edge
 * callback WHILE HOLDING g_irq.mu -- the single mutex shared by every
 * open GPIO handle.  A callback that called alp_gpio_close() or
 * alp_gpio_irq_disable() on its OWN pin (or on any OTHER open pin --
 * g_irq.mu is global) re-locked that same mutex on the same thread and
 * deadlocked.  The fix moves the callback invocation OUTSIDE g_irq.mu
 * (re-validating the slot + reading the event UNDER the lock, then
 * calling the callback after releasing it) -- see
 * peripheral_gpio.c's irq_dispatcher().
 *
 * Unlike the CAN/RPC backends, the shared GPIO dispatcher thread is
 * detached once at first alp_gpio_irq_enable() and never joined, so
 * there is no self-join hazard here to pair with the mutex fix -- see
 * this file's header comment in peripheral_gpio.c for why moving the
 * callback outside the lock is sufficient on its own for this
 * particular backend.
 *
 * This file #includes the real backend .c file directly (same
 * technique as tests/yocto/peripheral_gpio_irq_rollback.c) to reach
 * its file-local `struct alp_gpio` / pool_acquire() and the
 * g_gpio_test_set_config_hook seam, and drives the REAL shared IRQ
 * dispatcher thread (irq_dispatcher()) against a pipe standing in for
 * the /dev/gpiochipN line_fd -- writing a `struct gpio_v2_line_event`-
 * sized chunk into the write end makes poll()/read() on the read end
 * behave exactly like a real edge event without touching a real GPIO
 * line.
 *
 * Two scenarios:
 *   1. test_self_close_from_callback_no_deadlock -- a pin's own
 *      callback calls alp_gpio_close() on itself.  Must not hang (would
 *      indicate the mutex-reentrancy deadlock) -- run under
 *      ThreadSanitizer (see tests/yocto/CMakeLists.txt's
 *      alp_test_peripheral_gpio_tsan target) to also prove no data
 *      race.
 *   2. test_cross_pin_close_from_callback_no_deadlock -- pin A's
 *      callback calls alp_gpio_irq_disable() + alp_gpio_close() on a
 *      DIFFERENT pin B -- g_irq.mu is one global lock shared by every
 *      handle, so this is a distinct code path from the self-close
 *      above (the closer and the closed pin differ) and must not
 *      deadlock either.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_gpio_self_close
 *   ctest --test-dir build -R alp_test_peripheral_gpio_self_close
 */

#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include "test_assert.h"

#include "../../src/yocto/peripheral_gpio.c"

#define TEST_TIMEOUT_MS 5000

static int fake_set_config(int line_fd, const struct gpio_v2_line_config *cfg)
{
	(void)line_fd;
	(void)cfg;
	return 0; /* always "succeeds" against the fake line */
}

static void reset_fixture(void)
{
	g_gpio_test_set_config_hook       = fake_set_config;
	g_gpio_test_fail_dispatcher_start = 0;
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

static void sleep_ms(long ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

static bool wait_until(atomic_int *flag, int timeout_ms)
{
	int waited_ms = 0;
	while (!atomic_load(flag)) {
		sleep_ms(1);
		if (++waited_ms >= timeout_ms) {
			return false;
		}
	}
	return true;
}

/* Fire one edge "event" at `pin`: a poll()able, sizeof(struct
 * gpio_v2_line_event)-shaped write into its pipe write-end -- the
 * dispatcher's read() only cares about byte count, not content (see
 * this file's header comment). */
static void fire_event(int write_fd)
{
	struct gpio_v2_line_event ev;
	memset(&ev, 0, sizeof(ev));
	ssize_t w = write(write_fd, &ev, sizeof(ev));
	ALP_ASSERT_EQ_INT((size_t)w, sizeof(ev));
}

/* ------------------------------------------------------------------ */
/* 1. A pin's own callback closes itself.                              */
/* ------------------------------------------------------------------ */

static atomic_int g_self_cb_entered;
static atomic_int g_self_cb_returned;

static void self_close_cb(alp_gpio_t *pin, void *user)
{
	(void)user;
	atomic_store(&g_self_cb_entered, 1);
	/* THE self-close under test: pre-#756 this deadlocked re-locking
     * g_irq.mu on the dispatcher thread that already (used to) hold it
     * across this very callback. */
	alp_gpio_close(pin);
	atomic_store(&g_self_cb_returned, 1);
}

static void test_self_close_from_callback_no_deadlock(void)
{
	reset_fixture();
	atomic_store(&g_self_cb_entered, 0);
	atomic_store(&g_self_cb_returned, 0);

	int              pipe_fds[2];
	struct alp_gpio *pin = open_fake_pin(pipe_fds);
	ALP_ASSERT_TRUE(pin != NULL);

	alp_status_t rc = alp_gpio_irq_enable(pin, ALP_GPIO_EDGE_RISING, self_close_cb, NULL);
	ALP_ASSERT_EQ_INT(rc, ALP_OK);

	fire_event(pipe_fds[1]);

	/* If self_close_cb()'s alp_gpio_close() ever regresses to the
     * pre-#756 lock-held-across-callback shape, this hangs instead of
     * completing -- wait_until()'s bound turns that into a clean test
     * failure rather than a wedged CI job. */
	ALP_ASSERT_TRUE(wait_until(&g_self_cb_entered, TEST_TIMEOUT_MS));
	ALP_ASSERT_TRUE(wait_until(&g_self_cb_returned, TEST_TIMEOUT_MS));

	/* pool_release() (inside alp_gpio_close()) already closed pipe_fds[0]. */
	(void)close(pipe_fds[1]);
}

/* ------------------------------------------------------------------ */
/* 2. Pin A's callback closes a DIFFERENT pin B.                        */
/* ------------------------------------------------------------------ */

static atomic_int       g_cross_cb_entered;
static atomic_int       g_cross_cb_returned;
static struct alp_gpio *g_cross_pin_b;

static void cross_close_cb(alp_gpio_t *pin, void *user)
{
	(void)pin;
	(void)user;
	atomic_store(&g_cross_cb_entered, 1);
	/* g_irq.mu is ONE global lock shared by every handle -- closing a
     * DIFFERENT pin from within this callback exercises the same
     * re-entrancy path as the self-close above, just against a
     * different slot. */
	(void)alp_gpio_irq_disable(g_cross_pin_b);
	alp_gpio_close(g_cross_pin_b);
	atomic_store(&g_cross_cb_returned, 1);
}

static void noop_cb(alp_gpio_t *pin, void *user)
{
	(void)pin;
	(void)user;
}

static void test_cross_pin_close_from_callback_no_deadlock(void)
{
	reset_fixture();
	atomic_store(&g_cross_cb_entered, 0);
	atomic_store(&g_cross_cb_returned, 0);

	int              pipe_a[2], pipe_b[2];
	struct alp_gpio *pin_a = open_fake_pin(pipe_a);
	struct alp_gpio *pin_b = open_fake_pin(pipe_b);
	ALP_ASSERT_TRUE(pin_a != NULL && pin_b != NULL);
	g_cross_pin_b = pin_b;

	ALP_ASSERT_EQ_INT(alp_gpio_irq_enable(pin_a, ALP_GPIO_EDGE_RISING, cross_close_cb, NULL),
	                  ALP_OK);
	ALP_ASSERT_EQ_INT(alp_gpio_irq_enable(pin_b, ALP_GPIO_EDGE_RISING, noop_cb, NULL), ALP_OK);

	fire_event(pipe_a[1]);

	ALP_ASSERT_TRUE(wait_until(&g_cross_cb_entered, TEST_TIMEOUT_MS));
	ALP_ASSERT_TRUE(wait_until(&g_cross_cb_returned, TEST_TIMEOUT_MS));

	alp_gpio_close(pin_a);
	(void)close(pipe_a[1]);
	(void)close(pipe_b[1]);
}

int main(void)
{
	test_self_close_from_callback_no_deadlock();
	test_cross_pin_close_from_callback_no_deadlock();
	ALP_TEST_SUMMARY();
}
