/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #756: the Yocto SocketCAN backend's RX
 * reader thread (src/backends/can/yocto_drv.c) used to invoke filter
 * callbacks WHILE HOLDING d->lock, and its (pre-#756) single-phase
 * y_close() unconditionally pthread_join()'d the RX thread.  A filter
 * callback that called alp_can_close()/alp_can_remove_filter() on its
 * own handle therefore deadlocked twice over: re-locking d->lock on
 * the same thread, and (had it gotten past that) self-joining.  Both
 * are fixed together (moving only the callback outside the lock is
 * NOT sufficient by itself -- the issue's own warning): _dispatch_rx()
 * now snapshots callbacks under the lock and invokes them after
 * releasing it, and y_close() is split into y_shutdown()/y_destroy()
 * (can_ops.h), mirroring the RPC backend's GHSA-xhm8-7f87-93q5
 * redesign -- see that header's file comment.
 *
 * This file #includes the real backend .c file directly (same
 * technique as tests/yocto/rpc_yocto_self_close.c) to reach its
 * file-local `y_can_data_t` / `_ops` / `y_shutdown`/`y_destroy` /
 * `_rx_loop`.  Opening a real SocketCAN socket needs a live canN
 * interface this host doesn't have, so every test here builds a
 * `y_can_data_t` the same way y_open() does and drives `_rx_loop()`
 * over an AF_UNIX/SOCK_DGRAM socketpair standing in for the SocketCAN
 * fd -- `_dispatch_rx()` only ever calls read()/decode on raw bytes,
 * so a real can_frame-shaped write here reproduces the real decode +
 * dispatch path without a real CAN bus.
 *
 * @par Test doubles for the dispatcher machinery (deliberate)
 * Like rpc_yocto_self_close.c, this binary does not link alp::sdk (ODR
 * avoidance), so `alp_can_close_finalize()` (normally
 * src/can_dispatch.c) gets a minimal test-local definition below that
 * just runs y_destroy() -- the dispatcher's REAL active-op drain ahead
 * of that call, and its single-owner CAS, are exercised separately at
 * the true dispatcher layer by the existing tests/unit/can_registry
 * suite; this file's job is only the backend's shutdown()/destroy()
 * split.
 *
 * Three scenarios:
 *   1. test_self_close_no_uaf_no_selfjoin -- the issue reproduction: a
 *      filter callback closes its own handle.  Must not hang (would
 *      indicate a real self-join or the mutex-reentrancy deadlock) --
 *      run under ThreadSanitizer (see tests/yocto/CMakeLists.txt's
 *      alp_test_can_yocto_tsan target) to also prove no use-after-free
 *      / data race.
 *   2. test_external_close_without_callback_is_synchronous -- the
 *      ordinary, uncontested external-close path stays synchronous
 *      (joins the RX thread, returns DONE).
 *   3. test_concurrent_external_vs_self_close_is_single_shot -- an
 *      external close racing the handle's own self-close (via a
 *      callback) is single-shot across many iterations: exactly one
 *      side tears the handle down, the other is a safe no-op, neither
 *      hangs.
 *
 * Build + run (needs a Linux host):
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_can_yocto_self_close
 *   ctest --test-dir build -R alp_test_can_yocto_self_close
 */

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <linux/can.h>

#include "test_assert.h"

#include "../../src/backends/can/yocto_drv.c"

#define TEST_TIMEOUT_MS 5000
#define RACE_ITERATIONS 200

/* ------------------------------------------------------------------ */
/* Test double for the dispatcher's alp_can_close_finalize() (see this */
/* file's header comment)                                              */
/* ------------------------------------------------------------------ */

void alp_can_close_finalize(void *owner)
{
	alp_can_backend_state_t *st = (alp_can_backend_state_t *)owner;
	if (st == NULL) return;
	y_destroy(st);
}

/* Mirrors the dispatcher's DONE-path handling in alp_can_close():
 * shutdown(), and only a DONE result proceeds straight to destroy(). */
static void do_close(alp_can_backend_state_t *st)
{
	if (y_shutdown(st) == ALP_CAN_SHUTDOWN_DONE) {
		y_destroy(st);
	}
}

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

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

/* Build a y_can_data_t the same way y_open() does, minus the real
 * socket()/SIOCGIFINDEX/bind() dance -- `fd` stands in for the
 * SocketCAN socket (a socketpair end in every test below).  A single
 * "match everything" filter (can_id=0, can_mask=0) is installed
 * directly rather than through y_add_filter() (which wants a real
 * setsockopt(CAN_RAW_FILTER)). */
static y_can_data_t *
make_test_can(int fd, alp_can_backend_state_t *st, alp_can_rx_cb_t cb, void *user)
{
	y_can_data_t *d = (y_can_data_t *)calloc(1, sizeof(*d));
	ALP_ASSERT_TRUE(d != NULL);
	d->fd    = fd;
	d->owner = st;
	pthread_mutex_init(&d->lock, NULL);
	/* Real wake-pipe, exactly like y_open() creates -- _rx_loop() poll()s
     * it alongside `fd` (issue #756); a calloc'd {0,0} would alias stdin
     * instead of a real pipe. */
	ALP_ASSERT_EQ_INT(pipe(d->rx_wake_pipe), 0);
	d->filters[0].cb          = cb;
	d->filters[0].user        = user;
	d->filters[0].kf.can_id   = 0;
	d->filters[0].kf.can_mask = 0; /* matches every frame */
	d->filters[0].in_use      = true;

	st->be_data = d;
	st->ops     = &_ops;
	return d;
}

static void send_test_frame(int fd)
{
	struct can_frame cf;
	memset(&cf, 0, sizeof(cf));
	cf.can_id  = 0x123;
	cf.can_dlc = 1;
	cf.data[0] = 0xAA;
	ssize_t w  = send(fd, &cf, sizeof(cf), 0);
	ALP_ASSERT_EQ_INT(w, (ssize_t)sizeof(cf));
}

/* Spawn _rx_loop() through a thin wrapper that flips *worker_done to
 * true right BEFORE the thread actually terminates -- regardless of
 * whether it stays joinable (external-close path) or detaches itself
 * (self-close path).  Tests wait on this instead of inferring
 * completion from a side effect. */
struct rx_spawn_ctx {
	y_can_data_t *d;
	atomic_int   *worker_done;
};

static void *rx_thread_wrapper(void *arg)
{
	struct rx_spawn_ctx *ctx = (struct rx_spawn_ctx *)arg;
	void                *ret = _rx_loop(ctx->d);
	atomic_store(ctx->worker_done, 1);
	free(ctx);
	return ret;
}

static int spawn_rx_thread(y_can_data_t *d, atomic_int *worker_done)
{
	struct rx_spawn_ctx *ctx = (struct rx_spawn_ctx *)malloc(sizeof(*ctx));
	if (ctx == NULL) return -1;
	ctx->d           = d;
	ctx->worker_done = worker_done;
	atomic_store(worker_done, 0);
	d->rx_running = true;
	int rc        = pthread_create(&d->rx_thread, NULL, rx_thread_wrapper, ctx);
	if (rc != 0) {
		d->rx_running = false;
		free(ctx);
	}
	return rc;
}

/* ------------------------------------------------------------------ */
/* 1. The issue reproduction: a filter callback closes its OWN handle. */
/* ------------------------------------------------------------------ */

static atomic_int              g_cb_entered;
static atomic_int              g_cb_returned;
static atomic_int              g_worker_done;
static atomic_int              g_shutdown_result_bad;
static alp_can_backend_state_t g_self_close_state;

static void on_frame_close_me(const alp_can_frame_t *frame, void *user)
{
	(void)frame;
	(void)user;
	atomic_store(&g_cb_entered, 1);

	/* THE self-close under test: a filter callback running on this
     * handle's own RX thread closes its own handle.  Pre-#756 this
     * deadlocked re-locking d->lock (callbacks used to run WITH it
     * held) and, had it gotten past that, pthread_join()'d its own
     * thread.  Deliberately not an ALP_ASSERT_* call here -- this runs
     * on the RX thread, which would race the main thread's own
     * ALP_ASSERT_* calls on test_assert.h's unsynchronised counters;
     * record the outcome and let the main thread assert it after
     * joining/waiting. */
	alp_can_shutdown_result_t r = y_shutdown(&g_self_close_state);
	if (r != ALP_CAN_SHUTDOWN_DEFERRED) {
		atomic_store(&g_shutdown_result_bad, 1);
	}
	atomic_store(&g_cb_returned, 1);
}

static void test_self_close_no_uaf_no_selfjoin(void)
{
	atomic_store(&g_cb_entered, 0);
	atomic_store(&g_cb_returned, 0);
	atomic_store(&g_shutdown_result_bad, 0);

	int sv[2];
	ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), 0);

	memset(&g_self_close_state, 0, sizeof(g_self_close_state));
	y_can_data_t *d          = make_test_can(sv[0], &g_self_close_state, on_frame_close_me, NULL);
	g_self_close_state.owner = &g_self_close_state;

	ALP_ASSERT_EQ_INT(spawn_rx_thread(d, &g_worker_done), 0);

	send_test_frame(sv[1]);

	/* If y_shutdown() ever regresses to a real self-join or the old
     * lock-held-across-callback shape, this hangs instead of
     * returning -- wait_until()'s bound turns that into a clean test
     * failure rather than a wedged CI job. */
	ALP_ASSERT_TRUE(wait_until(&g_cb_entered, TEST_TIMEOUT_MS));
	ALP_ASSERT_TRUE(wait_until(&g_cb_returned, TEST_TIMEOUT_MS));
	ALP_ASSERT_TRUE(atomic_load(&g_shutdown_result_bad) == 0);

	/* Wait for the worker's own thread function to actually return
     * (detach + destroy + exit) -- the DEFERRED path's epilogue (this
     * file's alp_can_close_finalize() stand-in) runs on this same
     * thread, strictly after the callback above returned. */
	ALP_ASSERT_TRUE(wait_until(&g_worker_done, TEST_TIMEOUT_MS));

	close(sv[1]);
}

/* ------------------------------------------------------------------ */
/* 2. Ordinary external close (no callback) stays synchronous.         */
/* ------------------------------------------------------------------ */

static void noop_frame_cb(const alp_can_frame_t *frame, void *user)
{
	(void)frame;
	(void)user;
}

static void test_external_close_without_callback_is_synchronous(void)
{
	atomic_int worker_done;

	int sv[2];
	ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), 0);

	alp_can_backend_state_t st;
	memset(&st, 0, sizeof(st));
	y_can_data_t *d = make_test_can(sv[0], &st, noop_frame_cb, NULL);
	st.owner        = &st;

	ALP_ASSERT_EQ_INT(spawn_rx_thread(d, &worker_done), 0);

	/* Uncontested external close: y_shutdown() must join + return DONE
     * before do_close() calls y_destroy(). */
	do_close(&st);

	ALP_ASSERT_TRUE(wait_until(&worker_done, TEST_TIMEOUT_MS));
	close(sv[1]);
}

/* ------------------------------------------------------------------ */
/* 3. External close racing a callback self-close: single-shot.        */
/* ------------------------------------------------------------------ */

static pthread_barrier_t        g_race_barrier;
static alp_can_backend_state_t *g_race_state;
static atomic_int               g_close_owner_claimed;

static bool claim_close_once(void)
{
	int expected = 0;
	return atomic_compare_exchange_strong(&g_close_owner_claimed, &expected, 1);
}

static void on_frame_close_me_race(const alp_can_frame_t *frame, void *user)
{
	(void)frame;
	(void)user;
	pthread_barrier_wait(&g_race_barrier);
	/* Single-owner election test double (see this file's header
     * comment): in production this is the dispatcher's one atomic CAS
     * (src/can_dispatch.c's alp_can_close()) upstream of shutdown() --
     * y_shutdown() itself assumes at most one caller ever reaches it
     * per handle. */
	if (claim_close_once()) {
		if (y_shutdown(g_race_state) == ALP_CAN_SHUTDOWN_DONE) {
			y_destroy(g_race_state);
		}
		/* DEFERRED: _rx_loop()'s epilogue (this same thread, once this
         * callback returns) completes the teardown. */
	}
}

static void test_concurrent_external_vs_self_close_is_single_shot(void)
{
	for (int i = 0; i < RACE_ITERATIONS; ++i) {
		atomic_int worker_done;
		int        sv[2];
		ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), 0);

		alp_can_backend_state_t st;
		memset(&st, 0, sizeof(st));
		y_can_data_t *d = make_test_can(sv[0], &st, on_frame_close_me_race, NULL);
		st.owner        = &st;
		g_race_state    = &st;
		atomic_store(&g_close_owner_claimed, 0);

		pthread_barrier_init(&g_race_barrier, NULL, 2);
		ALP_ASSERT_EQ_INT(spawn_rx_thread(d, &worker_done), 0);

		send_test_frame(sv[1]);
		/* Give the frame time to be dispatched into on_frame_close_me_race()
         * so it is already waiting at the barrier before racing our own
         * close against it. */
		sleep_ms(5);

		pthread_barrier_wait(&g_race_barrier);
		if (claim_close_once()) {
			do_close(&st);
		}

		ALP_ASSERT_TRUE(wait_until(&worker_done, TEST_TIMEOUT_MS));

		close(sv[1]);
		pthread_barrier_destroy(&g_race_barrier);
	}
}

int main(void)
{
	test_self_close_no_uaf_no_selfjoin();
	test_external_close_without_callback_is_synchronous();
	test_concurrent_external_vs_self_close_is_single_shot();
	ALP_TEST_SUMMARY();
}
