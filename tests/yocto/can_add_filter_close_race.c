/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for the dev-review follow-up on issue #756: the
 * #756 patch's original alp_can_close() called the backend's
 * shutdown() (which snapshots "does an RX thread exist, and is it me"
 * via y_can_data_t::rx_running) BEFORE draining active_ops, regressing
 * the drain-first invariant issue #629 established for every close()
 * in this SDK (CAS -> drain -> teardown).
 *
 * Reachable interleaving this file reproduces deterministically (see
 * g_can_test_add_filter_prespawn_hook in src/backends/can/yocto_drv.c):
 *   - Thread A calls alp_can_add_filter() -- the FIRST EVER filter add
 *     on this handle, so it lazily spawns the RX thread.  It passes
 *     the dispatcher's op_enter() (active_ops=1), then y_add_filter()
 *     parks it at the hook, BEFORE it takes d->lock or spawns anything.
 *   - Thread B (this test) calls alp_can_close().  With the drain-
 *     before-shutdown BUG, close() would call y_shutdown() right away:
 *     d->rx_running is still false (A hasn't reached the spawn), so
 *     y_shutdown() decides "no thread, nothing to wake" and reports
 *     DONE.  close() would THEN drain active_ops -- correctly waiting
 *     for A to finish -- but by the time A finishes it HAS spawned the
 *     RX thread (rx_running=true) that y_shutdown() never learned
 *     about: destroy() frees `d` and closes the fd while that thread
 *     is live and blocked in poll() on both -- a use-after-free and
 *     the exact close()-vs-concurrent-poll() hazard the wake-pipe
 *     exists to prevent.
 *   - The FIX drains active_ops BEFORE ever calling shutdown(), so by
 *     the time shutdown() runs, A has unconditionally finished --
 *     including the spawn -- and the snapshot is race-free.
 *
 * Drives the REAL public alp_can_add_filter()/alp_can_close() (not a
 * hand-rolled stand-in) by #including both src/can_dispatch.c and
 * src/backends/can/yocto_drv.c directly into this TU (same technique
 * as tests/yocto/can_yocto_self_close.c for the backend alone, and
 * tests/yocto/rpc_dispatch_close_race.c for the dispatcher alone) --
 * this is the one test in the suite that needs BOTH layers' file-local
 * internals (struct alp_can's pool allocator, y_can_data_t, the two
 * test hooks) in the same translation unit, so it bypasses
 * alp_can_open()/y_open() (which need a real canN interface) and
 * constructs the handle directly, then drives everything else through
 * the real alp_can_add_filter()/alp_can_close() entry points.
 * g_can_test_apply_filters_hook stands in for the real
 * setsockopt(CAN_RAW_FILTER) (the socketpair fd standing in for the
 * SocketCAN socket cannot accept that call for real).
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_can_add_filter_close_race
 *   ctest --test-dir build -R alp_test_can_add_filter_close_race
 * Under ThreadSanitizer: see alp_test_can_gpio_tsan in this directory's
 * CMakeLists.txt (this target is wired into the same sanitizer run as
 * the other CAN/GPIO #756 regressions).
 */

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <linux/can.h>

#include "test_assert.h"

#include "../../src/can_dispatch.c"
#include "../../src/backends/can/yocto_drv.c"

#define TEST_TIMEOUT_MS 5000
#define RACE_ITERATIONS 20

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

/* Stand-in for the real setsockopt(CAN_RAW_FILTER) -- see this file's
 * header comment. */
static int fake_apply_filters(y_can_data_t *d, const struct can_filter *set, size_t n)
{
	(void)d;
	(void)set;
	(void)n;
	return 0; /* always "succeeds" against the fake socketpair fd */
}

static void noop_rx_cb(const alp_can_frame_t *frame, void *user)
{
	(void)frame;
	(void)user;
}

static atomic_int g_hook_entered;

/* Parks the add_filter thread here -- BEFORE it takes d->lock / spawns
 * the RX thread -- long enough for the racing alp_can_close() below to
 * have already entered (and, on the buggy ordering, wrongly finished
 * with) its shutdown()/drain sequence. */
static void prespawn_hook(void)
{
	atomic_store(&g_hook_entered, 1);
	sleep_ms(100);
}

static atomic_int g_add_filter_done;
static alp_can_t *g_race_handle;

static void *add_filter_thread(void *arg)
{
	(void)arg;
	alp_can_filter_t filter = { .id = 0, .mask = 0, .ext_id = false };
	int32_t          id     = -1;
	(void)alp_can_add_filter(g_race_handle, &filter, noop_rx_cb, NULL, &id);
	atomic_store(&g_add_filter_done, 1);
	return NULL;
}

static void test_close_races_first_add_filter_mid_spawn(void)
{
	g_can_test_apply_filters_hook = fake_apply_filters;

	for (int iter = 0; iter < RACE_ITERATIONS; ++iter) {
		atomic_store(&g_hook_entered, 0);
		atomic_store(&g_add_filter_done, 0);
		g_can_test_add_filter_prespawn_hook = prespawn_hook;

		struct alp_can *h = _alloc();
		ALP_ASSERT_TRUE(h != NULL);
		g_race_handle = h;

		int sv[2];
		ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), 0);

		y_can_data_t *d = (y_can_data_t *)calloc(1, sizeof(*d));
		ALP_ASSERT_TRUE(d != NULL);
		d->fd    = sv[0];
		d->owner = h;
		pthread_mutex_init(&d->lock, NULL);
		ALP_ASSERT_EQ_INT(pipe(d->rx_wake_pipe), 0);

		h->state.ops     = &_ops;
		h->state.be_data = d;
		h->state.owner   = h;
		alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);

		pthread_t t;
		ALP_ASSERT_EQ_INT(pthread_create(&t, NULL, add_filter_thread, NULL), 0);

		/* Let the add_filter thread genuinely enter the hook (and thus
         * be counted in active_ops, mid-op) before racing close()
         * against it. */
		ALP_ASSERT_TRUE(wait_until(&g_hook_entered, TEST_TIMEOUT_MS));

		/* THE race under test: close() while add_filter() is still
         * parked BEFORE it has spawned the RX thread.  On the fixed
         * dispatcher, alp_can_close()'s drain blocks here until
         * add_filter() has fully finished (including the spawn) before
         * shutdown()/destroy() ever run -- see this file's header
         * comment for what the pre-fix ordering did instead. */
		alp_can_close(h);

		ALP_ASSERT_TRUE(wait_until(&g_add_filter_done, TEST_TIMEOUT_MS));
		ALP_ASSERT_EQ_INT(pthread_join(t, NULL), 0);

		close(sv[1]);
		g_can_test_add_filter_prespawn_hook = NULL;
	}

	g_can_test_apply_filters_hook = NULL;
}

int main(void)
{
	test_close_races_first_add_filter_mid_spawn();
	ALP_TEST_SUMMARY();
}
