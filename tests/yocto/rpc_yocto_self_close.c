/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for GHSA-xhm8-7f87-93q5: the Yocto rpc backend's
 * rx worker (src/backends/rpc/yocto_drv.c) invokes subscriber
 * callbacks directly from its poll()/read() thread.  A callback that
 * calls alp_rpc_close() on its OWN channel used to make y_close()
 * pthread_join() its own rx thread (guaranteed EDEADLK, silently
 * ignored) and then free the channel anyway, while rpc_rx_main() kept
 * dereferencing the freed block after the callback returned -- a
 * deterministic heap use-after-free.
 *
 * This file #includes src/backends/rpc/yocto_drv.c directly (rather
 * than linking the built library) so it can reach the CVE's exact
 * file-local surface: `struct rpc_be`, `y_open`/`y_subscribe`/
 * `y_close`, and the `rpc_rx_main` worker.  Opening a channel through
 * y_open()'s real /dev/rpmsg_ctrl0 + RPMSG_CREATE_EPT_IOCTL dance
 * needs live remoteproc hardware this host doesn't have, so instead
 * every test here builds a `struct rpc_be` the same way y_open() does
 * and drives rpc_rx_main() over a AF_UNIX/SOCK_DGRAM socketpair
 * standing in for the /dev/rpmsgN chardev.  This is safe: the real
 * backend code (frame_build/frame_parse/rpc_rx_main/y_close/...) only
 * ever calls POSIX read()/write()/poll()/ioctl() + pthread -- never a
 * libmetal/open-amp user-space symbol -- so no OpenAMP library needs
 * to be installed to exercise it here.
 *
 * Three scenarios:
 *   1. test_self_close_no_uaf_no_selfjoin -- the CVE reproduction: a
 *      subscriber callback closes its own channel.  Must not hang
 *      (would indicate a real self-join) and must not crash (would
 *      indicate the use-after-free) -- run this binary under ASan/TSan
 *      to turn the latter into a hard failure rather than a maybe-crash.
 *   2. test_external_close_without_callback_is_synchronous -- the
 *      ordinary, uncontested external-close path must be unchanged:
 *      still join-based and synchronous.
 *   3. test_concurrent_external_vs_self_close_is_single_shot -- an
 *      external alp_rpc_close() racing the channel's own self-close
 *      (via a callback) must be single-shot: exactly one of the two
 *      tears the channel down, the other is a safe no-op, and neither
 *      hangs nor double-frees, across many iterations.
 *
 * Build + run (needs a Linux host; no OpenAMP/libmetal install
 * required):
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_rpc_yocto_self_close
 *   ctest --test-dir build -R alp_test_rpc_yocto_self_close
 *
 * Under ASan/TSan (recommended -- this is what actually proves no
 * use-after-free / no data race, as opposed to "didn't crash today"):
 *   cmake -B build-asan -DALP_OS=yocto -DALP_BUILD_TESTS=ON \
 *         -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g -O1"
 *   cmake --build build-asan --target alp_test_rpc_yocto_self_close
 *   ./build-asan/tests/yocto/alp_test_rpc_yocto_self_close
 *
 *   cmake -B build-tsan -DALP_OS=yocto -DALP_BUILD_TESTS=ON \
 *         -DCMAKE_C_FLAGS="-fsanitize=thread -g -O1"
 *   cmake --build build-tsan --target alp_test_rpc_yocto_self_close
 *   ./build-tsan/tests/yocto/alp_test_rpc_yocto_self_close
 */

#define ALP_SDK_HAVE_OPENAMP_USERLAND 1
#include "../../src/backends/rpc/yocto_drv.c"

#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "test_assert.h"

#define TEST_TIMEOUT_MS 5000
#define RACE_ITERATIONS 200

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

static void sleep_ms(long ms)
{
	struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
	nanosleep(&ts, NULL);
}

/* Construct a channel the same way y_open() does, minus the real
 * /dev/rpmsg_ctrl0 ioctl dance -- `ept_fd` stands in for the chardev
 * (a socketpair end in every test below). */
static struct rpc_be *make_test_channel(int ept_fd)
{
	struct rpc_be *ch = (struct rpc_be *)calloc(1, sizeof(*ch));
	ALP_ASSERT_TRUE(ch != NULL);
	if (ch == NULL) {
		return NULL;
	}

	strncpy(ch->name, "selfclose", sizeof(ch->name) - 1);
	pthread_mutex_init(&ch->tx_mutex, NULL);
	pthread_mutex_init(&ch->sub_mutex, NULL);
	pthread_mutex_init(&ch->call_mutex, NULL);
	pthread_cond_init(&ch->call_cond, NULL);
	ch->ept_fd  = ept_fd;
	ch->ctrl_fd = -1;
	ALP_ASSERT_EQ_INT(pipe(ch->rx_wake_pipe), 0);
	atomic_store(&ch->rx_run, 1);
	return ch;
}

/* Poll-wait (bounded) for a flag another thread sets.  Every test below
 * bounds every wait so a regression to the original self-join deadlock
 * fails this test (timeout) instead of hanging CI forever. */
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

/* Once a channel tears itself down, rpc_rx_main()'s epilogue closes
 * ept_fd (our socketpair end).  Detect that from the OTHER end without
 * touching `ch` again -- `ch` may be concurrently freed on the rx
 * thread the instant the self-close path finishes, so the test must
 * never dereference it again after triggering a self-close. */
static bool wait_peer_closed(int peer_fd, int timeout_ms)
{
	int waited_ms = 0;
	while (waited_ms < timeout_ms) {
		struct pollfd pfd = { .fd = peer_fd, .events = 0 };
		int           rc  = poll(&pfd, 1, 10);
		if (rc > 0 && (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
			return true;
		}
		char probe = 0;
		if (send(peer_fd, &probe, 1, MSG_NOSIGNAL) < 0 &&
		    (errno == ECONNREFUSED || errno == EPIPE || errno == EBADF)) {
			return true;
		}
		waited_ms += 10;
	}
	return false;
}

/* Spawn rpc_rx_main() through a thin wrapper that flips *worker_done to
 * true right BEFORE the thread actually terminates -- regardless of
 * whether the thread stays joinable (external-close path, unchanged)
 * or detaches itself (self-close path -- see y_close()'s from_worker
 * branch and rpc_rx_main()'s epilogue).  Tests wait on this instead of
 * inferring "the worker is done" from a side effect like the peer fd
 * closing: that happens partway through the teardown, before the
 * thread's final detach + free + return, so racing ahead of it (e.g.
 * reusing a per-iteration stack variable or a shared global the very
 * next loop iteration) is a TEST-HARNESS race, not a production one --
 * exactly the class of false positive an early version of this file
 * hit under ThreadSanitizer. */
struct rx_spawn_ctx {
	struct rpc_be *ch;
	atomic_int    *worker_done;
};

static void *rx_thread_wrapper(void *arg)
{
	struct rx_spawn_ctx *ctx = (struct rx_spawn_ctx *)arg;
	void                *ret = rpc_rx_main(ctx->ch);
	atomic_store(ctx->worker_done, 1);
	free(ctx);
	return ret;
}

static int spawn_rx_thread(struct rpc_be *ch, atomic_int *worker_done)
{
	struct rx_spawn_ctx *ctx = (struct rx_spawn_ctx *)malloc(sizeof(*ctx));
	if (ctx == NULL) {
		return -1;
	}
	ctx->ch          = ch;
	ctx->worker_done = worker_done;
	atomic_store(worker_done, 0);
	int rc = pthread_create(&ch->rx_thread, NULL, rx_thread_wrapper, ctx);
	if (rc != 0) {
		free(ctx);
	}
	return rc;
}

/* ------------------------------------------------------------------ */
/* 1. The CVE reproduction: a callback closes its OWN channel.         */
/* ------------------------------------------------------------------ */

static atomic_int              g_cb_entered;
static atomic_int              g_cb_returned;
static atomic_int              g_worker_done;
static alp_rpc_backend_state_t g_self_close_state;

static void on_close_me(const void *payload, size_t len, void *user)
{
	(void)payload;
	(void)len;
	(void)user;
	atomic_store(&g_cb_entered, 1);

	/* THE self-close under test: a subscriber callback running on this
     * channel's own rx thread closes its own channel.  Pre-fix this
     * pthread_join()'d ch->rx_thread (itself -> EDEADLK, ignored) and
     * freed `ch`, so rpc_rx_main()'s use of `ch` after this call
     * returned was a use-after-free. */
	y_close(&g_self_close_state);

	atomic_store(&g_cb_returned, 1);
}

static void test_self_close_no_uaf_no_selfjoin(void)
{
	atomic_store(&g_cb_entered, 0);
	atomic_store(&g_cb_returned, 0);

	int sv[2];
	ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), 0);

	struct rpc_be *ch          = make_test_channel(sv[0]);
	g_self_close_state.be_data = ch;
	g_self_close_state.ops     = &_ops;

	ALP_ASSERT_EQ_INT(y_subscribe(&g_self_close_state, "close_me", on_close_me, ch), ALP_OK);
	ALP_ASSERT_EQ_INT(spawn_rx_thread(ch, &g_worker_done), 0);

	uint8_t frame[ALP_RPC_TX_FRAME_MAX];
	int     built = frame_build(frame, sizeof frame, "close_me", NULL, 0);
	ALP_ASSERT_TRUE(built > 0);
	ssize_t w = send(sv[1], frame, (size_t)built, 0);
	ALP_ASSERT_EQ_INT(w, built);

	/* If y_close() ever regresses to a real self-join, this hangs
     * instead of returning -- wait_until()'s bound turns that into a
     * clean test failure rather than a wedged CI job. */
	ALP_ASSERT_TRUE(wait_until(&g_cb_entered, TEST_TIMEOUT_MS));
	ALP_ASSERT_TRUE(wait_until(&g_cb_returned, TEST_TIMEOUT_MS));

	/* The dispatcher must not see this channel as open anymore the
     * moment the (self-)close claims ownership. */
	ALP_ASSERT_NULL(rpc_be_data_load(&g_self_close_state));

	/* rpc_rx_main()'s epilogue must complete the deferred teardown
     * (closing the chardev fd) exactly once after the loop unwinds --
     * ASan/TSan is what actually proves no use-after-free happened;
     * this only proves forward progress and single teardown. */
	ALP_ASSERT_TRUE(wait_peer_closed(sv[1], TEST_TIMEOUT_MS));

	/* Wait for the worker's own thread function to actually return
     * (detach + exit) before this test function returns, so nothing
     * outlives it. */
	ALP_ASSERT_TRUE(wait_until(&g_worker_done, TEST_TIMEOUT_MS));

	close(sv[1]);
}

/* ------------------------------------------------------------------ */
/* 2. Ordinary external close (no callback) stays synchronous.         */
/* ------------------------------------------------------------------ */

static void test_external_close_without_callback_is_synchronous(void)
{
	atomic_int worker_done;

	int sv[2];
	ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), 0);

	struct rpc_be          *ch = make_test_channel(sv[0]);
	alp_rpc_backend_state_t st = { .be_data = ch, .ops = &_ops };

	ALP_ASSERT_EQ_INT(spawn_rx_thread(ch, &worker_done), 0);

	/* Uncontested external close: y_close() must join + tear down
     * before returning, so the peer fd is already closed the instant
     * this call comes back -- no polling/waiting needed. */
	y_close(&st);

	ALP_ASSERT_NULL(rpc_be_data_load(&st));
	ALP_ASSERT_TRUE(wait_peer_closed(sv[1], /*timeout_ms=*/50));
	/* y_close()'s external-owner path joins ch->rx_thread itself, so
     * the worker is provably done already; this is a cheap sanity
     * check that spawn_rx_thread()'s wrapper agrees. */
	ALP_ASSERT_TRUE(wait_until(&worker_done, TEST_TIMEOUT_MS));

	close(sv[1]);
}

/* ------------------------------------------------------------------ */
/* 3. External close racing a callback self-close: single-shot.        */
/* ------------------------------------------------------------------ */

static pthread_barrier_t        g_race_barrier;
static alp_rpc_backend_state_t *g_race_state;

static void on_close_me_race(const void *payload, size_t len, void *user)
{
	(void)payload;
	(void)len;
	(void)user;
	/* Rendezvous with the main thread so both sides call y_close() on
     * the same channel as close together in time as the scheduler
     * allows. */
	pthread_barrier_wait(&g_race_barrier);
	y_close(g_race_state);
}

static void test_concurrent_external_vs_self_close_is_single_shot(void)
{
	for (int i = 0; i < RACE_ITERATIONS; ++i) {
		atomic_int worker_done;
		int        sv[2];
		ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), 0);

		struct rpc_be          *ch = make_test_channel(sv[0]);
		alp_rpc_backend_state_t st = { .be_data = ch, .ops = &_ops };
		g_race_state               = &st;

		pthread_barrier_init(&g_race_barrier, NULL, 2);
		ALP_ASSERT_EQ_INT(y_subscribe(&st, "close_me", on_close_me_race, ch), ALP_OK);
		ALP_ASSERT_EQ_INT(spawn_rx_thread(ch, &worker_done), 0);

		uint8_t frame[ALP_RPC_TX_FRAME_MAX];
		int     built = frame_build(frame, sizeof frame, "close_me", NULL, 0);
		ssize_t w     = send(sv[1], frame, (size_t)built, 0);
		ALP_ASSERT_EQ_INT(w, built);

		/* Give the frame time to be dispatched into on_close_me_race()
         * so it is already waiting at the barrier before we race our
         * own close against it. */
		sleep_ms(5);

		pthread_barrier_wait(&g_race_barrier);
		y_close(&st); /* external close, racing the callback's self-close */

		/* Single-owner: regardless of which side won, the channel must
         * be closed from the dispatcher's point of view and the peer
         * fd released -- exactly once, no hang. */
		ALP_ASSERT_NULL(rpc_be_data_load(&st));
		ALP_ASSERT_TRUE(wait_peer_closed(sv[1], TEST_TIMEOUT_MS));

		/* Wait for the worker thread to have FULLY terminated (whether
         * it was the self-close owner running the deferred epilogue
         * teardown, or the race's loser returning immediately) before
         * the next iteration reuses `g_race_barrier` / this stack frame
         * -- reusing that memory earlier is exactly the test-harness
         * race an earlier version of this file tripped under
         * ThreadSanitizer (the worker can still be inside
         * rpc_rx_main()'s epilogue after the fd is already closed). */
		ALP_ASSERT_TRUE(wait_until(&worker_done, TEST_TIMEOUT_MS));

		close(sv[1]);
		pthread_barrier_destroy(&g_race_barrier);
	}
}

/* ------------------------------------------------------------------ */

int main(void)
{
	test_self_close_no_uaf_no_selfjoin();
	test_external_close_without_callback_is_synchronous();
	test_concurrent_external_vs_self_close_is_single_shot();
	ALP_TEST_SUMMARY();
}
