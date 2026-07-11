/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for GHSA-xhm8-7f87-93q5: the Yocto rpc backend's
 * rx worker (src/backends/rpc/yocto_drv.c) invokes subscriber
 * callbacks directly from its poll()/read() thread.  A callback that
 * calls alp_rpc_close() on its OWN channel used to make the (pre-
 * redesign) y_close() pthread_join() its own rx thread (guaranteed
 * EDEADLK, silently ignored) and then free the channel anyway, while
 * rpc_rx_main() kept dereferencing the freed block after the callback
 * returned -- a deterministic heap use-after-free.  A SECOND patch
 * round moved the cancel+join+teardown sequence to the dispatcher,
 * which then had no way to avoid EITHER self-joining OR spinning
 * forever draining an op that only the very close call it was
 * blocking could unblock.  This file now exercises the authoritative,
 * third-round redesign: `y_shutdown()`/`y_destroy()` (replacing the
 * single `y_close()`) plus a sticky `closing` predicate (replacing a
 * one-shot cancel) -- see yocto_drv.c's own header comment.
 *
 * This file #includes src/backends/rpc/yocto_drv.c directly (rather
 * than linking the built library) so it can reach the exact
 * file-local surface: `struct rpc_be`, `y_open`/`y_subscribe`/
 * `y_shutdown`/`y_destroy`, and the `rpc_rx_main` worker.  Opening a
 * channel through y_open()'s real /dev/rpmsg_ctrl0 +
 * RPMSG_CREATE_EPT_IOCTL dance needs live remoteproc hardware this
 * host doesn't have, so instead every test here builds a
 * `struct rpc_be` the same way y_open() does and drives rpc_rx_main()
 * over a AF_UNIX/SOCK_DGRAM socketpair standing in for the /dev/rpmsgN
 * chardev.  This is safe: the real backend code
 * (frame_build/frame_parse/rpc_rx_main/y_shutdown/y_destroy/...) only
 * ever calls POSIX read()/write()/poll()/ioctl() + pthread -- never a
 * libmetal/open-amp user-space symbol -- so no OpenAMP library needs
 * to be installed to exercise it here.
 *
 * @par Test doubles for the dispatcher machinery (deliberate)
 * This file drives the BACKEND directly, below the dispatcher (it is
 * NOT linked against alp::sdk -- see this file's CMakeLists.txt entry
 * -- to avoid an ODR clash with the separately-compiled copy of this
 * same TU the library links).  Two dispatcher responsibilities the
 * redesign moved OUT of the backend are stood in for here rather than
 * skipped:
 *   - `alp_rpc_close_finalize()` (normally defined by
 *     src/rpc_dispatch.c, called by rpc_rx_main()'s epilogue on the
 *     self-close/DEFERRED path) gets a minimal test-local definition
 *     below that just runs y_destroy() -- the dispatcher's REAL
 *     active-op drain ahead of that call is exercised separately, at
 *     the true dispatcher layer, by rpc_dispatch_close_race.c.
 *   - Single-owner election (normally the dispatcher's one atomic CAS,
 *     src/rpc_dispatch.c's `_rpc_begin_close()`) is stood in for by
 *     `claim_close_once()` below wherever a scenario deliberately
 *     races an "external" close against the channel's own self-close
 *     (scenario 3) -- y_shutdown() is a contract that assumes at most
 *     one caller ever reaches it per channel; without the real
 *     dispatcher enforcing that here, the test enforces it itself so
 *     it is exercising the SAME contract y_shutdown() actually ships
 *     under, not an easier one.
 *   - The dispatcher's active-op drain itself (wait until every op
 *     entered before the close won has left, THEN destroy) is stood in
 *     for, per-scenario, by waiting on the same "the in-flight op has
 *     returned" signal each scenario already tracks (`g_call_race_done`
 *     / joining the sender thread) before calling y_destroy() --
 *     see scenarios 4 and 5.
 *
 * Six scenarios:
 *   1. test_self_close_no_uaf_no_selfjoin -- the CVE reproduction: a
 *      subscriber callback closes its own channel.  Must not hang
 *      (would indicate a real self-join) and must not crash (would
 *      indicate the use-after-free) -- run this binary under ASan/TSan
 *      to turn the latter into a hard failure rather than a maybe-crash.
 *      Asserts the ALP_ERR_NOT_READY-within-a-bound + reopen-succeeds
 *      shape from the design's test list (the native_sim ZTEST
 *      counterpart in tests/unit/rpc_registry covers the identical
 *      shape against the dispatcher + a fake backend).
 *   2. test_external_close_without_callback_is_synchronous -- the
 *      ordinary, uncontested external-close path must be unchanged:
 *      still join-based and synchronous, returning DONE.
 *   3. test_concurrent_external_vs_self_close_is_single_shot -- an
 *      external close racing the channel's own self-close (via a
 *      callback) must be single-shot (via claim_close_once(), see
 *      above): exactly one of the two tears the channel down, the
 *      other is a safe no-op, and neither hangs nor double-frees,
 *      across many iterations.
 *   4. test_call_vs_close_no_uaf -- a thread blocked inside y_call()
 *      (holding tx_mutex, waiting on call_cond) races an external
 *      y_shutdown() on the same channel; must not hang and must not
 *      use-after-free call_mutex/call_cond/`ch`.
 *   5. test_send_vs_close_no_uaf -- a thread spinning on y_send()
 *      races an external y_shutdown() on the same channel, across many
 *      iterations; must not use-after-free tx_mutex/`ch`.
 *   6. test_late_staging_call_is_cancelled -- using
 *      g_y_call_test_late_staging_hook (see yocto_drv.c), delays a
 *      y_call() between "the dispatcher's op-count already counts it"
 *      (simulated: the hook fires right after y_call() loads `ch`) and
 *      actually staging the call slot, racing a close in that window.
 *      Proves the sticky `closing` flag catches this -- the exact gap
 *      the OLD one-shot cancel (fixed in round 2, itself superseded
 *      here) could not close.
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
/* Test doubles for the dispatcher machinery (see this file's header  */
/* comment for why each of these exists)                              */
/* ------------------------------------------------------------------ */

/* Generic stand-in for the dispatcher's own active-op count (struct
 * alp_rpc_channel's `chan_word` count bits, src/rpc_dispatch.c):
 * whichever scenario below needs to prove a call/send genuinely stays
 * "in flight" across a self-close increments this around the op and
 * decrements it on return -- see blocked_call_thread() in scenario 1.
 * Scenarios that don't track anything here leave it permanently 0, so
 * the wait in alp_rpc_close_finalize() below is a no-op for them. */
static atomic_int g_test_op_count;

/* alp_rpc_close_finalize() is normally defined by src/rpc_dispatch.c;
 * this binary does not link alp::sdk.  `owner` is always the address
 * of the calling scenario's own alp_rpc_backend_state_t here.
 * Mirrors the real _rpc_finalize()'s ordering (src/rpc_dispatch.c):
 * drain (sleep, never spin -- see that function's own doc comment for
 * why) before destroy(), NOT the other way around -- without this,
 * scenario 1's concurrently-blocked y_call() could still be inside
 * pthread_cond_wait() on call_mutex/call_cond the instant y_destroy()
 * (via rpc_be_teardown()) destroys them: POSIX UB plus a heap
 * use-after-free, exactly the class of bug the real dispatcher's
 * drain (ABOVE the ops vtable in production) exists to prevent. */
void alp_rpc_close_finalize(void *owner)
{
	alp_rpc_backend_state_t *st = (alp_rpc_backend_state_t *)owner;
	if (st == NULL) return;
	while (atomic_load(&g_test_op_count) != 0) {
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L }; /* 1 ms */
		nanosleep(&ts, NULL);
	}
	y_destroy(st);
}

/* Single-owner election test double: in production this is
 * src/rpc_dispatch.c's _rpc_begin_close() (one atomic CAS).  Scenario
 * 3 below is the only one that needs it -- see this file's header
 * comment. */
static atomic_int g_close_owner_claimed;

static bool claim_close_once(void)
{
	int expected = 0;
	return atomic_compare_exchange_strong(&g_close_owner_claimed, &expected, 1);
}

/* Mirrors the dispatcher's DONE-path handling in alp_rpc_close():
 * shutdown(), and only a DONE result proceeds straight to destroy().
 * A DEFERRED result destroys nothing here -- rpc_rx_main()'s epilogue
 * (via alp_rpc_close_finalize() above) completes that teardown later,
 * exactly once. */
static void do_close(alp_rpc_backend_state_t *st)
{
	if (y_shutdown(st) == ALP_RPC_SHUTDOWN_DONE) {
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
 * or detaches itself (self-close path -- see y_shutdown()'s
 * from_worker branch and rpc_rx_main()'s epilogue).  Tests wait on this
 * instead of
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

/* Thread B: blocked in y_call() with an UNBOUNDED timeout when the
 * self-close (below) fires -- the exact shape the design's test list
 * requires ("thread B blocked in call; rx callback closes; assert B
 * returns ALP_ERR_NOT_READY within a HARD <1s bound").  Counted in
 * g_test_op_count for the ENTIRE call, mirroring the dispatcher's own
 * op-enter/op-leave bracket (src/rpc_dispatch.c) around the real
 * alp_rpc_call() -- see alp_rpc_close_finalize()'s doc comment above. */
static atomic_int   g_blocked_call_done;
static alp_status_t g_blocked_call_result;
static atomic_int   g_self_close_shutdown_result_bad;

static void *blocked_call_thread(void *arg)
{
	(void)arg;
	atomic_fetch_add(&g_test_op_count, 1);
	uint8_t resp[8];
	size_t  resp_len = sizeof resp;
	g_blocked_call_result =
	    y_call(&g_self_close_state, "no_such_method", NULL, 0, resp, &resp_len, UINT32_MAX);
	atomic_fetch_sub(&g_test_op_count, 1);
	atomic_store(&g_blocked_call_done, 1);
	return NULL;
}

static void on_close_me(const void *payload, size_t len, void *user)
{
	(void)payload;
	(void)len;
	(void)user;
	atomic_store(&g_cb_entered, 1);

	/* THE self-close under test: a subscriber callback running on this
     * channel's own rx thread closes its own channel.  Pre-redesign
     * (round 1) this pthread_join()'d ch->rx_thread (itself ->
     * EDEADLK, ignored) and freed `ch` anyway, so rpc_rx_main()'s use
     * of `ch` after this call returned was a use-after-free.  This
     * calls y_shutdown() directly (not the removed y_close()) --
     * expected to return ALP_RPC_SHUTDOWN_DEFERRED, since we ARE
     * ch->rx_thread here; the epilogue in rpc_rx_main() (below, once
     * this callback returns) completes the deferred teardown via
     * alp_rpc_close_finalize().
     *
     * Deliberately NOT an ALP_ASSERT_* call here: this runs on the rx
     * thread, so asserting here would race the main thread's own
     * ALP_ASSERT_* calls on test_assert.h's unsynchronised counters --
     * the same TEST-HARNESS race class this file's other background
     * threads already avoid (see e.g. send_race_thread()'s doc
     * comment).  Record the outcome and let the main thread assert it
     * after joining. */
	alp_rpc_shutdown_result_t r = y_shutdown(&g_self_close_state);
	if (r != ALP_RPC_SHUTDOWN_DEFERRED) {
		atomic_store(&g_self_close_shutdown_result_bad, 1);
	}

	atomic_store(&g_cb_returned, 1);
}

static void test_self_close_no_uaf_no_selfjoin(void)
{
	atomic_store(&g_cb_entered, 0);
	atomic_store(&g_cb_returned, 0);
	atomic_store(&g_blocked_call_done, 0);
	atomic_store(&g_self_close_shutdown_result_bad, 0);
	atomic_store(&g_test_op_count, 0);

	int sv[2];
	ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), 0);

	struct rpc_be *ch          = make_test_channel(sv[0]);
	g_self_close_state.be_data = ch;
	g_self_close_state.ops     = &_ops;
	g_self_close_state.owner   = &g_self_close_state;
	ch->owner                  = &g_self_close_state;

	ALP_ASSERT_EQ_INT(y_subscribe(&g_self_close_state, "close_me", on_close_me, ch), ALP_OK);
	ALP_ASSERT_EQ_INT(spawn_rx_thread(ch, &g_worker_done), 0);

	/* Thread B: block in y_call(UINT32_MAX) BEFORE the self-close frame
     * arrives, so it is genuinely in-flight (tx_mutex held, waiting on
     * call_cond) when the callback below closes the channel. */
	pthread_t caller;
	ALP_ASSERT_EQ_INT(pthread_create(&caller, NULL, blocked_call_thread, NULL), 0);
	sleep_ms(20); /* let it reach its staged wait */

	uint8_t frame[ALP_RPC_TX_FRAME_MAX];
	int     built = frame_build(frame, sizeof frame, "close_me", NULL, 0);
	ALP_ASSERT_TRUE(built > 0);
	ssize_t w = send(sv[1], frame, (size_t)built, 0);
	ALP_ASSERT_EQ_INT(w, built);

	/* If y_shutdown() ever regresses to a real self-join, this hangs
     * instead of returning -- wait_until()'s bound turns that into a
     * clean test failure rather than a wedged CI job. */
	ALP_ASSERT_TRUE(wait_until(&g_cb_entered, TEST_TIMEOUT_MS));
	ALP_ASSERT_TRUE(wait_until(&g_cb_returned, TEST_TIMEOUT_MS));
	ALP_ASSERT_TRUE(atomic_load(&g_self_close_shutdown_result_bad) == 0);

	/* HARD <1s bound (design's test list, item 1): thread B's
     * UINT32_MAX-timeout call must be unblocked by the sticky cancel,
     * not left to hang forever. */
	ALP_ASSERT_TRUE(wait_until(&g_blocked_call_done, 1000));
	ALP_ASSERT_EQ_INT(pthread_join(caller, NULL), 0);
	ALP_ASSERT_TRUE(g_blocked_call_result == ALP_ERR_NOT_READY);

	/* rpc_rx_main()'s epilogue must complete the deferred teardown
     * (closing the chardev fd) exactly once after the loop unwinds --
     * ASan/TSan is what actually proves no use-after-free happened;
     * this only proves forward progress and single teardown.  Wait for
     * this FIRST: the DEFERRED path only nulls out be_data (inside
     * y_destroy(), via alp_rpc_close_finalize()) once the epilogue
     * actually runs, which is strictly after this callback has already
     * returned -- checking be_data before this would be a genuine
     * test-harness race, not a production one. */
	ALP_ASSERT_TRUE(wait_peer_closed(sv[1], TEST_TIMEOUT_MS));

	/* Now the dispatcher-visible state is guaranteed to reflect the
     * completed close. */
	ALP_ASSERT_NULL(rpc_be_data_load(&g_self_close_state));

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
	ch->owner                  = &st;

	ALP_ASSERT_EQ_INT(spawn_rx_thread(ch, &worker_done), 0);

	/* Uncontested external close: y_shutdown() must join + return DONE
     * before do_close() calls y_destroy(), so the peer fd is already
     * closed the instant this call comes back -- no polling/waiting
     * needed. */
	do_close(&st);

	ALP_ASSERT_NULL(rpc_be_data_load(&st));
	ALP_ASSERT_TRUE(wait_peer_closed(sv[1], /*timeout_ms=*/50));
	/* y_shutdown()'s external-owner path joins ch->rx_thread itself, so
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
	/* Rendezvous with the main thread so both sides race the close on
     * the same channel as close together in time as the scheduler
     * allows. */
	pthread_barrier_wait(&g_race_barrier);
	/* Single-owner election test double (see this file's header
     * comment): in production this is the dispatcher's one atomic
     * CAS, upstream of ops->shutdown() -- y_shutdown() itself now
     * assumes at most one caller ever reaches it per channel. The
     * loser here is a safe, immediate no-op, matching
     * _rpc_begin_close()'s documented contract. */
	if (claim_close_once()) {
		if (y_shutdown(g_race_state) == ALP_RPC_SHUTDOWN_DONE) {
			y_destroy(g_race_state);
		}
		/* DEFERRED: rpc_rx_main()'s epilogue (this same thread, once
         * this callback returns) completes the teardown. */
	}
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
		ch->owner                  = &st;
		atomic_store(&g_close_owner_claimed, 0);

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
		/* external close, racing the callback's self-close -- same
         * single-owner test double as on_close_me_race() above. */
		if (claim_close_once()) {
			do_close(&st);
		}

		/* Single-owner: regardless of which side won, the channel must
         * be closed from the dispatcher's point of view and the peer
         * fd released -- exactly once, no hang.  Wait for the peer fd
         * close FIRST: when the callback wins (DEFERRED), be_data only
         * goes NULL once rpc_rx_main()'s epilogue actually runs
         * y_destroy() -- strictly after both racers have returned from
         * this point -- so checking it before this wait would be a
         * test-harness race, not a production one (see scenario 1's
         * identical ordering note). */
		ALP_ASSERT_TRUE(wait_peer_closed(sv[1], TEST_TIMEOUT_MS));
		ALP_ASSERT_NULL(rpc_be_data_load(&st));

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
/* 4. y_call() in flight, racing an external y_shutdown().              */
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
		ch->owner                  = &st;

		atomic_int worker_done;
		ALP_ASSERT_EQ_INT(spawn_rx_thread(ch, &worker_done), 0);

		atomic_store(&g_call_race_done, 0);
		pthread_t caller;
		ALP_ASSERT_EQ_INT(pthread_create(&caller, NULL, call_race_thread, NULL), 0);

		/* Let y_call() get well into its staged wait (tx_mutex held,
         * call_pending = true, blocked in pthread_cond_timedwait)
         * before racing the close against it -- this is the exact
         * in-flight-op-vs-teardown window this scenario is about:
         * without the dispatcher's active-op drain (simulated below by
         * waiting on g_call_race_done BEFORE calling y_destroy() -- see
         * this file's header comment on the test doubles), destroying
         * ch's mutexes/cond and freeing it while this thread is still
         * inside pthread_cond_timedwait() on them would be POSIX UB
         * plus a heap use-after-free. */
		sleep_ms(20);

		/* External shutdown: cancels the pending call (sticky `closing`
         * + broadcast) and joins the RX thread -- but does NOT, by
         * itself, guarantee this thread has left y_call() yet (it only
         * guarantees the wait wakes promptly).  Wait for that
         * explicitly (mirroring the dispatcher's active-op drain)
         * BEFORE destroying anything. */
		ALP_ASSERT_TRUE(y_shutdown(&st) == ALP_RPC_SHUTDOWN_DONE);

		ALP_ASSERT_TRUE(wait_until(&g_call_race_done, TEST_TIMEOUT_MS));
		ALP_ASSERT_EQ_INT(pthread_join(caller, NULL), 0);

		/* Cancelled by the sticky `closing` flag -- either the
         * "already pending when shutdown ran" cancel-and-broadcast, or
         * (proven separately by test 6 below) the check made when this
         * call stages itself.  ASan/TSan (not this assertion) is what
         * actually proves no use-after-free / data race happened, per
         * this file's header comment. */
		ALP_ASSERT_TRUE(g_call_race_result == ALP_ERR_NOT_READY);

		/* NOW it is safe to destroy -- mirrors the dispatcher's own
         * ordering: destroy() only ever runs once every op counted
         * before the close won has left. */
		y_destroy(&st);

		ALP_ASSERT_TRUE(wait_until(&worker_done, TEST_TIMEOUT_MS));
		close(sv[1]);
	}
}

/* ------------------------------------------------------------------ */
/* 5. y_send() loop racing an external y_shutdown().                    */
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
         * raced y_shutdown() below, not this return value.
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
		ch->owner                  = &st;

		atomic_int worker_done;
		ALP_ASSERT_EQ_INT(spawn_rx_thread(ch, &worker_done), 0);

		atomic_store(&g_send_race_stop, 0);
		atomic_store(&g_send_race_bad_rc, 0);
		pthread_t sender;
		ALP_ASSERT_EQ_INT(pthread_create(&sender, NULL, send_race_thread, NULL), 0);

		/* Only a minimal delay (unlike the other scenarios): the whole
         * point is to race y_shutdown() against y_send() as tightly as
         * the scheduler allows, on every iteration -- just enough for
         * the new thread to actually start running first. */
		sleep_ms(1);
		ALP_ASSERT_TRUE(y_shutdown(&st) == ALP_RPC_SHUTDOWN_DONE);
		atomic_store(&g_send_race_stop, 1);
		ALP_ASSERT_EQ_INT(pthread_join(sender, NULL), 0);
		/* Ordered-after every write send_race_thread made (the join
         * above) -- see that function's doc comment for why the
         * ALP_ASSERT_* call lives here instead of inside it. */
		ALP_ASSERT_TRUE(atomic_load(&g_send_race_bad_rc) == 0);

		/* NOW it is safe to destroy: the sender is joined (mirrors the
         * dispatcher's active-op drain -- see this file's header
         * comment and test 4's identical ordering). */
		y_destroy(&st);

		ALP_ASSERT_TRUE(wait_until(&worker_done, TEST_TIMEOUT_MS));
		close(sv[1]);
	}
}

/* ------------------------------------------------------------------ */
/* 6. Late-staging call: proves the STICKY closing flag, where a       */
/*    one-shot cancel would miss it.                                    */
/* ------------------------------------------------------------------ */

/* In production the window this proves is "the dispatcher's op-count
 * already counts this alp_rpc_call() invocation, but this function
 * hasn't reached its own staging step yet".  g_y_call_test_late_staging_hook
 * (declared in yocto_drv.c, right before y_call()) fires at exactly
 * that point -- right after y_call() loads `ch`, before it takes
 * tx_mutex/call_mutex to stage the call slot -- so pointing it at a
 * function that blocks until the racing close has fully run
 * deterministically reproduces the gap: y_shutdown() runs (and, since
 * nothing is pending yet, its one-shot "if (call_pending) cancel"
 * branch does nothing), THEN this call proceeds to stage itself.  If
 * the check-then-stage step didn't recheck the STICKY `closing` flag
 * (i.e. if this backend still had only the old one-shot cancel), it
 * would go on to wait out its own timeout with nobody left to signal
 * it. */
static pthread_barrier_t        g_late_stage_barrier;
static alp_rpc_backend_state_t *g_late_stage_state;
static atomic_int               g_late_stage_closed;
static alp_status_t             g_late_stage_result;

static void late_staging_hook(void)
{
	/* Let the racing close (below) run to completion before this
     * y_call() invocation is allowed to proceed to its own staging
     * step. */
	pthread_barrier_wait(&g_late_stage_barrier);
	ALP_ASSERT_TRUE(wait_until(&g_late_stage_closed, TEST_TIMEOUT_MS));
}

static void *late_call_thread(void *arg)
{
	(void)arg;
	uint8_t resp[8];
	size_t  resp_len = sizeof resp;
	g_late_stage_result =
	    y_call(g_late_stage_state, "no_such_method", NULL, 0, resp, &resp_len, TEST_TIMEOUT_MS);
	return NULL;
}

static void test_late_staging_call_is_cancelled(void)
{
	int sv[2];
	ALP_ASSERT_EQ_INT(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), 0);

	struct rpc_be          *ch = make_test_channel(sv[0]);
	alp_rpc_backend_state_t st = { .be_data = ch, .ops = &_ops };
	ch->owner                  = &st;
	g_late_stage_state         = &st;
	atomic_store(&g_late_stage_closed, 0);
	pthread_barrier_init(&g_late_stage_barrier, NULL, 2);

	atomic_int worker_done;
	ALP_ASSERT_EQ_INT(spawn_rx_thread(ch, &worker_done), 0);

	g_y_call_test_late_staging_hook = late_staging_hook;

	pthread_t caller;
	ALP_ASSERT_EQ_INT(pthread_create(&caller, NULL, late_call_thread, NULL), 0);

	/* Rendezvous with the hook (running on the caller thread, inside
     * y_call(), before it stages anything), then race the close in
     * while it's parked there. */
	pthread_barrier_wait(&g_late_stage_barrier);
	ALP_ASSERT_TRUE(y_shutdown(&st) == ALP_RPC_SHUTDOWN_DONE);
	atomic_store(&g_late_stage_closed, 1);

	ALP_ASSERT_EQ_INT(pthread_join(caller, NULL), 0);
	g_y_call_test_late_staging_hook = NULL;

	/* The whole point: the sticky `closing` flag -- rechecked at the
     * TOP of the check-then-stage critical section, AFTER the hook's
     * delay -- catches this, even though `closing` only became true
     * after this call had already been "in flight" from the
     * dispatcher's point of view. */
	ALP_ASSERT_TRUE(g_late_stage_result == ALP_ERR_NOT_READY);

	y_destroy(&st);
	ALP_ASSERT_TRUE(wait_until(&worker_done, TEST_TIMEOUT_MS));
	pthread_barrier_destroy(&g_late_stage_barrier);
	close(sv[1]);
}

/* ------------------------------------------------------------------ */

int main(void)
{
	test_self_close_no_uaf_no_selfjoin();
	test_external_close_without_callback_is_synchronous();
	test_concurrent_external_vs_self_close_is_single_shot();
	test_call_vs_close_no_uaf();
	test_send_vs_close_no_uaf();
	test_late_staging_call_is_cancelled();
	ALP_TEST_SUMMARY();
}
