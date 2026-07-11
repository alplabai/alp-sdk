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
 * A followup review found the self-join/UAF fix above incomplete: an
 * application thread can be inside y_call()/y_send() (or
 * y_subscribe()/y_unsubscribe()) when a close runs, which used to race
 * rpc_be_teardown()'s mutex/cond destroy + free() the same way (POSIX
 * UB + heap use-after-free) -- scenarios 4 and 5 below reproduce that
 * (defect 1); see rpc_be_op_enter()/rpc_be_drain_inflight() in
 * src/backends/rpc/yocto_drv.c for the fix.
 *
 * Five scenarios:
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
 *   4. test_call_vs_close_no_uaf -- a thread blocked inside y_call()
 *      (holding tx_mutex, waiting on call_cond) races an external
 *      y_close() on the same channel; must not hang and must not
 *      use-after-free call_mutex/call_cond/`ch` (defect 1).
 *   5. test_send_vs_close_no_uaf -- a thread spinning on y_send()
 *      races an external y_close() on the same channel, across many
 *      iterations; must not use-after-free tx_mutex/`ch` (defect 1).
 *
 * Build + run (needs a Linux host; no OpenAMP/libmetal install
 * required):
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_rpc_yocto_self_close
 *   ctest --test-dir build -R alp_test_rpc_yocto_self_close
 *
 * Under ASan/TSan (recommended -- this is what actually proves no
 * use-after-free / no data race, as opposed to "didn't crash today"):
 * wired as the alp_test_rpc_asan_ubsan / alp_test_rpc_tsan CTest
 * targets (GHSA-xhm8-7f87-93q5 defect 3, see run_sanitized_rpc_tests.sh)
 * in this directory's CMakeLists.txt, or manually:
 *   cmake -B build-asan -DALP_OS=yocto -DALP_BUILD_TESTS=ON \
 *         -DCMAKE_C_FLAGS="-fsanitize=address,undefined -g -O1"
 *   cmake --build build-asan --target alp_test_rpc_yocto_self_close
 *   ./build-asan/tests/yocto/alp_test_rpc_yocto_self_close
 *
 *   cmake -B build-tsan -DALP_OS=yocto -DALP_BUILD_TESTS=ON \
 *         -DCMAKE_C_FLAGS="-fsanitize=thread -g -O1"
 *   cmake --build build-tsan --target alp_test_rpc_yocto_self_close
 *   ./build-tsan/tests/yocto/alp_test_rpc_yocto_self_close
 *   (some sandboxed/containerised hosts need ASLR disabled for TSan's
 *   fixed shadow-memory mapping: `setarch $(uname -m) -R ./…` -- see
 *   the CTest wiring's own comment.)
 */

#define ALP_SDK_HAVE_OPENAMP_USERLAND 1
#include "../../src/backends/rpc/yocto_drv.c"

#include <fcntl.h>
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

	/* Match y_open()'s real O_NONBLOCK ept_fd (see y_open() above):
     * scenarios 4/5 write() into this fd via y_call()/y_send() with
     * nobody draining the peer end, so a BLOCKING socketpair fd here
     * would wedge write() forever once its buffer fills -- a test-
     * harness deadlock, not a production bug (the real chardev is
     * always opened non-blocking).  Tests 1-3 never call y_send()/
     * y_call() so this is a no-op for them. */
	int fl = fcntl(ept_fd, F_GETFL, 0);
	if (fl >= 0) {
		(void)fcntl(ept_fd, F_SETFL, fl | O_NONBLOCK);
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
/* 4. y_call() in flight, racing an external y_close() (defect 1).      */
/* ------------------------------------------------------------------ */

#define CALL_CLOSE_RACE_ITERATIONS 20

static alp_rpc_backend_state_t *g_call_race_state;
static atomic_int               g_call_race_done;
static alp_status_t             g_call_race_result;

static void *call_race_thread(void *arg)
{
	(void)arg;
	uint8_t resp[8];
	size_t  resp_len = sizeof resp;
	/* Bounded so a regression that fails to cancel this call (instead
     * of hanging forever) still turns into a clean test failure rather
     * than a wedged CI job -- the actual bug under test is a
     * crash/use-after-free/data race under ASan/TSan, not this timeout
     * firing. */
	g_call_race_result =
	    y_call(g_call_race_state, "no_such_method", NULL, 0, resp, &resp_len, TEST_TIMEOUT_MS);
	atomic_store(&g_call_race_done, 1);
	return NULL;
}

static void test_call_vs_close_no_uaf(void)
{
	for (int i = 0; i < CALL_CLOSE_RACE_ITERATIONS; ++i) {
		int sv[2];
		ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), 0);

		struct rpc_be          *ch = make_test_channel(sv[0]);
		alp_rpc_backend_state_t st = { .be_data = ch, .ops = &_ops };
		g_call_race_state          = &st;

		atomic_int worker_done;
		ALP_ASSERT_EQ_INT(spawn_rx_thread(ch, &worker_done), 0);

		atomic_store(&g_call_race_done, 0);
		pthread_t caller;
		ALP_ASSERT_EQ_INT(pthread_create(&caller, NULL, call_race_thread, NULL), 0);

		/* Let y_call() get well into its staged wait (tx_mutex held,
         * call_pending = true, blocked in pthread_cond_timedwait)
         * before racing the close against it -- this is the exact
         * in-flight-op-vs-teardown window GHSA-xhm8-7f87-93q5 defect 1
         * is about: without rpc_be_op_enter()/rpc_be_drain_inflight(),
         * y_close() -> rpc_be_teardown() would destroy call_mutex/
         * call_cond and free `ch` while this thread is still inside
         * pthread_cond_timedwait() on them. */
		sleep_ms(20);

		/* External close: joins the RX thread, then (rpc_be_teardown())
         * drains inflight_ops -- this in-flight call counted itself via
         * rpc_be_op_enter() -- before touching any mutex/cond or
         * freeing `ch`. */
		y_close(&st);

		ALP_ASSERT_TRUE(wait_until(&g_call_race_done, TEST_TIMEOUT_MS));
		ALP_ASSERT_EQ_INT(pthread_join(caller, NULL), 0);

		/* Cancelled by the close's pending-call cancel (the expected,
         * common outcome), or -- in the unlikely case the call hadn't
         * staged `call_pending` yet when that one-shot cancel ran --
         * caught by rpc_be_drain_inflight()'s own repeated re-cancel
         * and still resolved to NOT_READY; either is a correct,
         * non-crashing outcome. ASan/TSan (not this assertion) is what
         * actually proves no use-after-free / data race happened, per
         * this file's header comment. */
		ALP_ASSERT_TRUE(g_call_race_result == ALP_ERR_NOT_READY);

		ALP_ASSERT_TRUE(wait_until(&worker_done, TEST_TIMEOUT_MS));
		close(sv[1]);
	}
}

/* ------------------------------------------------------------------ */
/* 5. y_send() loop racing an external y_close() (defect 1).           */
/* ------------------------------------------------------------------ */

#define SEND_CLOSE_RACE_ITERATIONS 50

static alp_rpc_backend_state_t *g_send_race_state;
static atomic_int               g_send_race_stop;
static atomic_int               g_send_race_bad_rc;

static void *send_race_thread(void *arg)
{
	(void)arg;
	uint8_t payload = 0x42u;
	while (!atomic_load(&g_send_race_stop)) {
		/* ALP_OK (raced ahead of the close), ALP_ERR_NOT_READY (the
         * close already unpublished the channel before this call
         * entered), ALP_ERR_BUSY (nobody drains the socketpair peer
         * end in this test double, so the non-blocking send buffer
         * fills up -- EAGAIN, same as a real backpressured RPMsg
         * queue), or ALP_ERR_IO (the peer fd got closed mid-write, a
         * benign ordering artefact of the socketpair test double) are
         * all correct outcomes here -- ASan/TSan is what actually
         * proves no use-after-free / data race happened while this
         * raced y_close() below, not this return value.
         *
         * Deliberately NOT an ALP_ASSERT_* call here: test_assert.h's
         * counters are plain, unsynchronised globals (this test suite
         * predates any multi-threaded ASSERT caller), so asserting
         * from this background thread would itself race the main
         * thread's own ALP_ASSERT_* calls (e.g. the pthread_create()
         * check right after spawning this thread) on those counters --
         * a TEST-HARNESS data race ThreadSanitizer flagged in an
         * earlier version of this file, distinct from anything this
         * test is actually trying to prove about the RPC backend.
         * Record the outcome instead and let the JOINING thread
         * (guaranteed ordered-after this thread's exit by
         * pthread_join()) do the one ALP_ASSERT_TRUE below. */
		alp_status_t rc = y_send(g_send_race_state, "noop", &payload, sizeof payload);
		if (!(rc == ALP_OK || rc == ALP_ERR_NOT_READY || rc == ALP_ERR_BUSY || rc == ALP_ERR_IO)) {
			atomic_store(&g_send_race_bad_rc, 1);
		}
	}
	return NULL;
}

static void test_send_vs_close_no_uaf(void)
{
	for (int i = 0; i < SEND_CLOSE_RACE_ITERATIONS; ++i) {
		int sv[2];
		ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), 0);

		struct rpc_be          *ch = make_test_channel(sv[0]);
		alp_rpc_backend_state_t st = { .be_data = ch, .ops = &_ops };
		g_send_race_state          = &st;

		atomic_int worker_done;
		ALP_ASSERT_EQ_INT(spawn_rx_thread(ch, &worker_done), 0);

		atomic_store(&g_send_race_stop, 0);
		atomic_store(&g_send_race_bad_rc, 0);
		pthread_t sender;
		ALP_ASSERT_EQ_INT(pthread_create(&sender, NULL, send_race_thread, NULL), 0);

		/* Only a minimal delay (unlike the other scenarios): the whole
         * point is to race y_close() against y_send() as tightly as
         * the scheduler allows, on every iteration -- just enough for
         * the new thread to actually start running first. */
		sleep_ms(1);
		y_close(&st);
		atomic_store(&g_send_race_stop, 1);
		ALP_ASSERT_EQ_INT(pthread_join(sender, NULL), 0);
		/* Ordered-after every write send_race_thread made (the join
         * above) -- see that function's doc comment for why the
         * ALP_ASSERT_* call lives here instead of inside it. */
		ALP_ASSERT_TRUE(atomic_load(&g_send_race_bad_rc) == 0);

		ALP_ASSERT_TRUE(wait_until(&worker_done, TEST_TIMEOUT_MS));
		close(sv[1]);
	}
}

/* ------------------------------------------------------------------ */

int main(void)
{
	test_self_close_no_uaf_no_selfjoin();
	test_external_close_without_callback_is_synchronous();
	test_concurrent_external_vs_self_close_is_single_shot();
	test_call_vs_close_no_uaf();
	test_send_vs_close_no_uaf();
	ALP_TEST_SUMMARY();
}
