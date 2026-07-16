/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GHSA-xhm8-7f87-93q5 regression coverage for the RZ/V2N UIO/OpenAMP
 * rpc backend (src/backends/rpc/yocto_uio_drv.c) -- the same
 * self-close-from-callback + concurrent-blocked-call scenarios
 * tests/yocto/rpc_yocto_self_close.c proves against the `/dev/rpmsg*`
 * backend, adapted to this backend's transport.
 *
 * @par Why this needs a transport double, and what stays real
 * yocto_uio_drv.c's close-protocol machinery
 * (y_call()/y_shutdown()/uio_ept_cb()/rpc_recv_enter()/rpc_recv_leave())
 * is exercised here VERBATIM (this file #includes the real .c file, the
 * same technique tests/yocto/rpc_yocto_self_close.c uses). Only two
 * things this host cannot provide are doubled:
 *
 *   1. The rpmsg send path. `y_call()`/`y_send()` call
 *      `rpmsg_trysend(&ch->ept, ...)`, which reads `ept->addr` /
 *      `ept->dest_addr` and dispatches through
 *      `ept->rdev->ops.send_offchannel_raw` (confirmed by reading
 *      lib/rpmsg/rpmsg.c's `rpmsg_send_offchannel_raw()` directly --
 *      it is a real vtable call, not something requiring live vring
 *      memory). `fake_send_offchannel_raw()` below stands in for that
 *      one function pointer; everything else about `ch->ept` is real.
 *   2. libmetal's shared IRQ-processing thread
 *      (lib/system/linux/irq.c), which normally invokes
 *      `uio_rproc_notify_isr()` -> `remoteproc_get_notification()` ->
 *      `uio_ept_cb()`. `fake_irq_worker()` below is a single
 *      long-lived thread that drains a one-frame mailbox and calls
 *      `uio_ept_cb()` directly -- the transport-agnostic stand-in for
 *      "the shared worker thread that dispatches this channel's rx
 *      callback", exactly filling the role
 *      `uio_rproc_notify_isr()`/libmetal's irq thread play in
 *      production. `y_shutdown()`'s `recv_active`/`recv_thread`
 *      self-close detection (this file's real code, unmodified) works
 *      identically whether the calling thread is libmetal's real irq
 *      thread or this fake stand-in.
 *
 * `make_test_channel()` builds a `struct rpc_be` by hand (mirroring
 * `y_open()`'s field setup) WITHOUT ever calling
 * `metal_device_open()`/`rpmsg_create_ept()`.  It DOES call the real
 * `remoteproc_init()` (a cheap, side-effect-free mutex-init + memset --
 * see lib/remoteproc/remoteproc.c) so `ch->rproc` is in a state the
 * real, unmodified `y_destroy()`/`rpc_be_teardown()` can safely tear
 * back down: that function unconditionally calls
 * `remoteproc_shutdown()`/`remoteproc_remove()` on `ch->rproc`, which
 * dereference `rproc->ops`/`rproc->lock` and would be unsafe (NULL
 * deref / uninitialised mutex) against an all-zero `ch->rproc`.  This
 * lets every scenario below route cleanup through the SAME
 * `y_destroy()` production code takes, the same boundary
 * rpc_yocto_self_close.c draws around its own test doubles (see that
 * file's header comment).
 *
 * Four scenarios (a focused subset of rpc_yocto_self_close.c's six --
 * the ones that exercise this backend's transport-specific pieces;
 * the remaining two (late-staging-call, send-vs-close) reduce to
 * identical logic already covered by
 * tests/yocto/rpc_yocto_self_close.c above the ops vtable, since
 * `y_call()`/`y_send()`'s sticky-`closing` staging logic is byte-for-
 * byte copied from that backend):
 *   1. test_self_close_no_uaf_no_selfjoin -- a subscriber callback
 *      closes its OWN channel from inside uio_ept_cb(), racing a
 *      concurrently-blocked UINT32_MAX y_call().
 *   2. test_external_close_without_callback_is_synchronous -- ordinary
 *      external close: unregisters + drains cb_active synchronously,
 *      returns DONE.
 *   3. test_concurrent_external_vs_self_close_is_single_shot --
 *      external close racing the channel's own self-close: exactly
 *      one side tears the channel down.
 *   4. test_call_vs_close_no_uaf -- a thread blocked in y_call() races
 *      an external y_shutdown() on the same channel.
 *
 * BUILD (aarch64 cross-compile, this backend's whole point -- see
 * yocto_uio_drv.c's own STATUS note): this test needs
 * ALP_SDK_HAVE_OPENAMP_USERLAND (real open-amp/libmetal headers) to
 * even compile, since it #includes the real backend .c file, but
 * never touches a real UIO device or the M33 peer -- it CANNOT run to
 * completion in a way that proves anything about real silicon; it
 * exists to keep the close-protocol logic itself regression-covered
 * off-hardware, exactly like rpc_yocto_self_close.c already does for
 * the sibling backend.
 *
 * Build + run (needs the cross-built open-amp + libmetal headers/libs
 * on the include/link path -- see this backend's CMakeLists.txt
 * wiring):
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_rpc_uio_self_close
 */

#define ALP_SDK_HAVE_OPENAMP_USERLAND 1
#include "../../src/backends/rpc/yocto_uio_drv.c"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#include "test_assert.h"

#define TEST_TIMEOUT_MS 5000

/* ------------------------------------------------------------------ */
/* Transport double 1: the rpmsg send path (see this file's header)    */
/* ------------------------------------------------------------------ */

static struct rpmsg_device g_fake_rdev;

static int fake_send_offchannel_raw(struct rpmsg_device *rdev,
                                    uint32_t             src,
                                    uint32_t             dst,
                                    const void          *data,
                                    int                  len,
                                    int                  wait)
{
	(void)rdev;
	(void)src;
	(void)dst;
	(void)data;
	(void)len;
	(void)wait;
	/* Every scenario below only cares that y_call()/y_send() observe a
	 * successful "on the wire" outcome -- the actual bytes never need
	 * to go anywhere for a close-protocol test. A real reply (when a
	 * scenario needs one) is delivered separately via
	 * deliver_frame_via_fake_irq_thread() below, standing in for the
	 * M33 peer's own response. */
	return RPMSG_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Transport double 2: libmetal's shared IRQ thread (see this file's    */
/* header)                                                              */
/* ------------------------------------------------------------------ */

struct fake_frame {
	struct rpc_be *ch;
	uint8_t        bytes[ALP_RPC_TX_FRAME_MAX];
	size_t         len;
};

static pthread_mutex_t   g_mailbox_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    g_mailbox_cond = PTHREAD_COND_INITIALIZER;
static struct fake_frame g_mailbox;
static bool              g_mailbox_full;
static atomic_int        g_worker_stop;
static pthread_t         g_worker;

/* Bumped once, AFTER alp_rpc_close_finalize() returns, every time
 * fake_irq_worker() completes a DEFERRED (self-close) teardown --
 * mirrors rpc_yocto_self_close.c's wait_peer_closed(): a test must wait
 * for POSITIVE evidence the deferred teardown actually finished before
 * asserting on its result (be_data == NULL etc.), since the finalize
 * itself runs asynchronously on this worker thread. */
static atomic_int g_worker_finalize_count;

/* Stands in for uio_rproc_notify_isr(): drains one frame at a time and
 * dispatches it through the REAL uio_ept_cb() -- everything from there
 * down (rpc_recv_enter/rpc_recv_leave/the call-slot match/the subscribe
 * dispatch/the DEFERRED epilogue check) is this backend's real,
 * unmodified code. */
static void *fake_irq_worker(void *arg)
{
	(void)arg;
	for (;;) {
		pthread_mutex_lock(&g_mailbox_lock);
		while (!g_mailbox_full && !atomic_load(&g_worker_stop)) {
			pthread_cond_wait(&g_mailbox_cond, &g_mailbox_lock);
		}
		if (atomic_load(&g_worker_stop) && !g_mailbox_full) {
			pthread_mutex_unlock(&g_mailbox_lock);
			break;
		}
		struct fake_frame f = g_mailbox;
		g_mailbox_full      = false;
		pthread_mutex_unlock(&g_mailbox_lock);

		(void)uio_ept_cb(NULL, f.bytes, f.len, 0, f.ch);

		/* Mirrors uio_rproc_notify_isr()'s own epilogue -- see this
		 * file's header comment. */
		pthread_mutex_lock(&f.ch->call_mutex);
		bool close_from_worker = f.ch->close_from_worker;
		pthread_mutex_unlock(&f.ch->call_mutex);
		if (close_from_worker) {
			alp_rpc_close_finalize(f.ch->owner);
			atomic_fetch_add(&g_worker_finalize_count, 1);
		}
	}
	return NULL;
}

static void deliver_frame_via_fake_irq_thread(struct rpc_be *ch,
                                              const char    *method,
                                              const void    *payload,
                                              size_t         payload_len)
{
	pthread_mutex_lock(&g_mailbox_lock);
	g_mailbox.ch = ch;
	g_mailbox.len =
	    (size_t)frame_build(g_mailbox.bytes, sizeof g_mailbox.bytes, method, payload, payload_len);
	g_mailbox_full = true;
	pthread_cond_signal(&g_mailbox_cond);
	pthread_mutex_unlock(&g_mailbox_lock);
}

/* ------------------------------------------------------------------ */
/* Dispatcher test doubles -- mirrors rpc_yocto_self_close.c's own      */
/* (see that file's header comment for the rationale)                   */
/* ------------------------------------------------------------------ */

static atomic_int g_test_op_count;

void alp_rpc_close_finalize(void *owner)
{
	alp_rpc_backend_state_t *st = (alp_rpc_backend_state_t *)owner;
	if (st == NULL) return;
	while (atomic_load(&g_test_op_count) != 0) {
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L };
		nanosleep(&ts, NULL);
	}
	y_destroy(st);
}

static atomic_int g_close_owner_claimed;

static bool claim_close_once(void)
{
	int expected = 0;
	return atomic_compare_exchange_strong(&g_close_owner_claimed, &expected, 1);
}

static void do_close(alp_rpc_backend_state_t *st)
{
	if (y_shutdown(st) == ALP_RPC_SHUTDOWN_DONE) {
		y_destroy(st);
	}
}

/* ------------------------------------------------------------------ */
/* Shared helpers                                                       */
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

/* Builds a `struct rpc_be` the same way y_open() does for the fields
 * this test touches, WITHOUT metal_device_open()/rpmsg_create_ept() --
 * see this file's header comment. `remoteproc_init()` itself IS called
 * for real (it is a cheap, side-effect-free mutex-init + memset, see
 * lib/remoteproc/remoteproc.c) so this test CAN route through the
 * real, unmodified `y_destroy()` -> `rpc_be_teardown()` for cleanup --
 * that function unconditionally calls `remoteproc_shutdown()`/
 * `remoteproc_remove()` on `ch->rproc`, which dereference `rproc->ops`
 * and `rproc->lock` and are only safe once `remoteproc_init()` has
 * actually run (an all-zero `ch->rproc` has a NULL `ops`, so
 * `remoteproc_remove()`'s `rproc->ops->remove` check would crash). */
static struct rpc_be *make_test_channel(void)
{
	struct rpc_be *ch = (struct rpc_be *)calloc(1, sizeof(*ch));
	ALP_ASSERT_TRUE(ch != NULL);
	if (ch == NULL) return NULL;

	strncpy(ch->name, "uio_selfclose", sizeof(ch->name) - 1);
	pthread_mutex_init(&ch->tx_mutex, NULL);
	pthread_mutex_init(&ch->sub_mutex, NULL);
	pthread_mutex_init(&ch->call_mutex, NULL);
	pthread_cond_init(&ch->call_cond, NULL);
	ch->mhu_irq = -1;

	ch->ept.rdev      = &g_fake_rdev;
	ch->ept.addr      = 0x401u;
	ch->ept.dest_addr = 0x402u;
	ch->ept.priv      = ch;

	ALP_ASSERT_TRUE(remoteproc_init(&ch->rproc, &g_rproc_ops, ch) == &ch->rproc);
	/* Mirrors y_open() -- rpc_be_teardown() now gates its
	 * remoteproc_shutdown()/remoteproc_remove() calls on this flag
	 * (alp-sdk #683 bench fix, see struct rpc_be's doc comment), so
	 * this test double must set it too or lose that teardown coverage. */
	ch->rproc_ready = true;
	return ch;
}

/* ------------------------------------------------------------------ */
/* 1. The CVE reproduction: a callback closes its OWN channel.          */
/* ------------------------------------------------------------------ */

static atomic_int              g_cb_entered;
static atomic_int              g_cb_returned;
static alp_rpc_backend_state_t g_self_close_state;
static atomic_int              g_blocked_call_done;
static alp_status_t            g_blocked_call_result;
static atomic_int              g_self_close_shutdown_result_bad;

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

	/* THE self-close under test -- see y_shutdown()'s own doc comment
	 * (this file's real, unmodified code) for the recv_active +
	 * recv_thread self-close detection this exercises. Not an
	 * ALP_ASSERT_* call here: this runs on fake_irq_worker's thread,
	 * which would race the main thread's unsynchronised test_assert.h
	 * counters (see rpc_yocto_self_close.c's identical note). */
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

	struct rpc_be *ch          = make_test_channel();
	g_self_close_state.be_data = ch;
	g_self_close_state.ops     = &_ops;
	g_self_close_state.owner   = &g_self_close_state;
	ch->owner                  = &g_self_close_state;

	ALP_ASSERT_EQ_INT(y_subscribe(&g_self_close_state, "close_me", on_close_me, ch), ALP_OK);

	pthread_t caller;
	ALP_ASSERT_EQ_INT(pthread_create(&caller, NULL, blocked_call_thread, NULL), 0);
	sleep_ms(20); /* let it reach its staged wait */

	int finalize_before = atomic_load(&g_worker_finalize_count);
	deliver_frame_via_fake_irq_thread(ch, "close_me", NULL, 0);

	ALP_ASSERT_TRUE(wait_until(&g_cb_entered, TEST_TIMEOUT_MS));
	ALP_ASSERT_TRUE(wait_until(&g_cb_returned, TEST_TIMEOUT_MS));
	ALP_ASSERT_TRUE(atomic_load(&g_self_close_shutdown_result_bad) == 0);

	/* HARD <1s bound: thread B's UINT32_MAX-timeout call must be
	 * unblocked by the sticky cancel, not left to hang forever. */
	ALP_ASSERT_TRUE(wait_until(&g_blocked_call_done, 1000));
	ALP_ASSERT_EQ_INT(pthread_join(caller, NULL), 0);
	ALP_ASSERT_TRUE(g_blocked_call_result == ALP_ERR_NOT_READY);

	/* Wait for POSITIVE evidence fake_irq_worker() actually finished the
	 * deferred teardown (see g_worker_finalize_count's doc comment)
	 * BEFORE asserting on its result -- checking be_data any earlier is
	 * a test-harness race, not a production one (mirrors
	 * rpc_yocto_self_close.c's identical wait_peer_closed() ordering
	 * note). */
	int waited_ms = 0;
	while (atomic_load(&g_worker_finalize_count) == finalize_before) {
		sleep_ms(1);
		ALP_ASSERT_TRUE(++waited_ms < TEST_TIMEOUT_MS);
	}
	ALP_ASSERT_NULL(rpc_be_data_load(&g_self_close_state));
}

/* ------------------------------------------------------------------ */
/* 2. Ordinary external close (no callback) stays synchronous.          */
/* ------------------------------------------------------------------ */

static void test_external_close_without_callback_is_synchronous(void)
{
	struct rpc_be          *ch = make_test_channel();
	alp_rpc_backend_state_t st = { .be_data = ch, .ops = &_ops };
	ch->owner                  = &st;

	do_close(&st);

	ALP_ASSERT_NULL(rpc_be_data_load(&st));
}

/* ------------------------------------------------------------------ */
/* 3. External close racing a callback self-close: single-shot.         */
/* ------------------------------------------------------------------ */

static pthread_barrier_t        g_race_barrier;
static alp_rpc_backend_state_t *g_race_state;

static void on_close_me_race(const void *payload, size_t len, void *user)
{
	(void)payload;
	(void)len;
	(void)user;
	pthread_barrier_wait(&g_race_barrier);
	if (claim_close_once()) {
		if (y_shutdown(g_race_state) == ALP_RPC_SHUTDOWN_DONE) {
			y_destroy(g_race_state);
		}
	}
}

#define RACE_ITERATIONS 50

static void test_concurrent_external_vs_self_close_is_single_shot(void)
{
	for (int i = 0; i < RACE_ITERATIONS; ++i) {
		struct rpc_be          *ch = make_test_channel();
		alp_rpc_backend_state_t st = { .be_data = ch, .ops = &_ops };
		g_race_state               = &st;
		ch->owner                  = &st;
		atomic_store(&g_close_owner_claimed, 0);

		pthread_barrier_init(&g_race_barrier, NULL, 2);
		ALP_ASSERT_EQ_INT(y_subscribe(&st, "close_me", on_close_me_race, ch), ALP_OK);

		deliver_frame_via_fake_irq_thread(ch, "close_me", NULL, 0);

		sleep_ms(5); /* let the frame reach on_close_me_race()'s barrier wait */

		pthread_barrier_wait(&g_race_barrier);
		if (claim_close_once()) {
			do_close(&st);
		}

		/* Give the (possibly DEFERRED) worker-side teardown a moment to
		 * finish before the next iteration reuses g_race_barrier. */
		sleep_ms(20);
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
	g_call_race_result =
	    y_call(g_call_race_state, "no_such_method", NULL, 0, resp, &resp_len, TEST_TIMEOUT_MS);
	atomic_store(&g_call_race_done, 1);
	return NULL;
}

static void test_call_vs_close_no_uaf(void)
{
	for (int i = 0; i < CALL_CLOSE_RACE_ITERATIONS; ++i) {
		struct rpc_be          *ch = make_test_channel();
		alp_rpc_backend_state_t st = { .be_data = ch, .ops = &_ops };
		g_call_race_state          = &st;
		ch->owner                  = &st;

		atomic_store(&g_call_race_done, 0);
		pthread_t caller;
		ALP_ASSERT_EQ_INT(pthread_create(&caller, NULL, call_race_thread, NULL), 0);

		sleep_ms(20); /* let y_call() reach its staged wait */

		ALP_ASSERT_TRUE(y_shutdown(&st) == ALP_RPC_SHUTDOWN_DONE);

		ALP_ASSERT_TRUE(wait_until(&g_call_race_done, TEST_TIMEOUT_MS));
		ALP_ASSERT_EQ_INT(pthread_join(caller, NULL), 0);
		ALP_ASSERT_TRUE(g_call_race_result == ALP_ERR_NOT_READY);

		y_destroy(&st);
	}
}

/* ------------------------------------------------------------------ */
/* 5. alp-sdk #735: ALP_UIO_MHU_KICK_SLOT override boundary checks.     */
/* ------------------------------------------------------------------ */

static void test_mhu_kick_slot_default(void)
{
	unsetenv("ALP_UIO_MHU_KICK_SLOT");
	uint32_t out = 0;
	ALP_ASSERT_TRUE(mhu_kick_slot(&out));
	ALP_ASSERT_EQ_INT((int)out, (int)ALP_MHU_NS_CH5_KICK_SLOT);
}

static void test_mhu_kick_slot_valid_override(void)
{
	setenv("ALP_UIO_MHU_KICK_SLOT", "0x100", 1);
	uint32_t out = 0;
	ALP_ASSERT_TRUE(mhu_kick_slot(&out));
	ALP_ASSERT_EQ_INT((int)out, 0x100);
	unsetenv("ALP_UIO_MHU_KICK_SLOT");
}

static void test_mhu_kick_slot_rejects_unaligned(void)
{
	setenv("ALP_UIO_MHU_KICK_SLOT", "0x101", 1);
	uint32_t out = 0xDEADBEEFu;
	ALP_ASSERT_TRUE(!mhu_kick_slot(&out));
	/* *out left UNCHANGED on rejection (alp_checked_arith.h's documented
	 * contract, mirrored here by mhu_kick_slot()). */
	ALP_ASSERT_EQ_INT((int)out, (int)0xDEADBEEFu);
	unsetenv("ALP_UIO_MHU_KICK_SLOT");
}

static void test_mhu_kick_slot_rejects_u32_truncation(void)
{
	/* 0x100000000 (2^32) parses cleanly as an `unsigned long` on this
	 * 64-bit host (no ERANGE) but does not fit a uint32_t -- pre-#735 this
	 * was cast straight to uint32_t and silently truncated to 0, a wrong
	 * slot the caller would then dereference through instead of being
	 * rejected. */
	const char *env = "0x100000000";

	setenv("ALP_UIO_MHU_KICK_SLOT", env, 1);
	uint32_t out = 0xDEADBEEFu;
	ALP_ASSERT_TRUE(!mhu_kick_slot(&out));
	ALP_ASSERT_EQ_INT((int)out, (int)0xDEADBEEFu);
	unsetenv("ALP_UIO_MHU_KICK_SLOT");
}

/* NOTE (honesty, not a claim of ERANGE-specific coverage): on this 64-bit
 * host `strtoul()` overflow ALWAYS saturates to ULONG_MAX (2^64-1, C11
 * 7.22.1.4p8), and ULONG_MAX cast to size_t is always > UINT32_MAX, so this
 * input is rejected identically whether it goes through mhu_kick_slot()'s
 * `errno == ERANGE` check (yocto_uio_drv.c) or falls through to
 * alp_size_to_u32()'s narrowing check -- the two branches are NOT
 * distinguishable by any strtoul()-overflowing input on a host where
 * `unsigned long` is 64 bits. This test proves overflow inputs are
 * rejected end-to-end (a real regression: pre-#735 there was no errno
 * check at all, so ULONG_MAX silently became (uint32_t)ULONG_MAX ==
 * 0xFFFFFFFF, a huge slot the caller would dereference through); it does
 * NOT prove the `errno == ERANGE` line specifically is what does the
 * rejecting -- that line only matters on a host where `unsigned long` is
 * 32 bits (e.g. LLP64), where ULONG_MAX IS u32-representable and would
 * otherwise sail through alp_size_to_u32() unrejected. */
static void test_mhu_kick_slot_rejects_ulong_overflow(void)
{
	const char *env = "0x10000000000000000"; /* overflows a 64-bit unsigned long */

	setenv("ALP_UIO_MHU_KICK_SLOT", env, 1);
	uint32_t out = 0xDEADBEEFu;
	ALP_ASSERT_TRUE(!mhu_kick_slot(&out));
	ALP_ASSERT_EQ_INT((int)out, (int)0xDEADBEEFu);
	unsetenv("ALP_UIO_MHU_KICK_SLOT");
}

static void test_mhu_kick_slot_rejects_garbage(void)
{
	setenv("ALP_UIO_MHU_KICK_SLOT", "not-a-number", 1);
	uint32_t out = 0;
	ALP_ASSERT_TRUE(mhu_kick_slot(&out)); /* unparseable -> safe default, unchanged from pre-fix */
	ALP_ASSERT_EQ_INT((int)out, (int)ALP_MHU_NS_CH5_KICK_SLOT);
	unsetenv("ALP_UIO_MHU_KICK_SLOT");
}

/* alp-sdk #735: drive the SHIPPED call site (uio_rproc_notify()) itself,
 * not just the shared alp_u32_add_checked()/alp_size_range_valid() helpers
 * it calls underneath -- a prior version of this test called those helpers
 * directly with the same inputs uio_rproc_notify() uses, which stayed green
 * even with the entire bounds-check block deleted from the shipped
 * function. A fake `struct metal_device`/`metal_io_region` pair stands in
 * for the real UIO_MHU/UIO_MHU_SHM mappings (no metal_device_open() needed,
 * mirroring make_test_channel()'s "double only what this host can't
 * provide" rule from this file's header comment) so `uio_rproc_notify()`
 * runs unmodified against them. Both fake regions use a REAL, fully-sized
 * backing buffer -- only the reported `metal_io_region.size` is shrunk --
 * so a reverted/broken bounds check can never walk off this test's own
 * memory; the assertions below, not a crash, are what catch the
 * regression. */

static void fake_region_init(struct metal_device *dev, void *virt, size_t size)
{
	memset(dev, 0, sizeof(*dev));
	dev->num_regions     = 1;
	dev->regions[0].virt = virt;
	dev->regions[0].size = size;
}

static void test_notify_bounds_reject_out_of_mapping(void)
{
	unsetenv("ALP_UIO_MHU_KICK_SLOT");

	uint8_t mhu_backing[0x1000];
	uint8_t shm_backing[0x1000];
	memset(mhu_backing, 0, sizeof mhu_backing);
	memset(shm_backing, 0, sizeof shm_backing);

	struct metal_device mhu_dev;
	struct metal_device shm_dev;
	/* Reported mapping stops exactly at the default kick slot (0xA0), so
	 * MSG_INT_SET's register (0xA0+0x04) starts exactly at the (reported)
	 * mapping boundary -- offset == capacity is only valid for a
	 * zero-length request, #735's exact boundary case. */
	fake_region_init(&mhu_dev, mhu_backing, ALP_MHU_NS_CH5_KICK_SLOT);
	fake_region_init(&shm_dev, shm_backing, sizeof shm_backing);

	struct rpc_be ch;
	memset(&ch, 0, sizeof ch);
	ch.dev[UIO_MHU_SHM] = &shm_dev;
	ch.dev[UIO_MHU]     = &mhu_dev;

	struct remoteproc rproc;
	memset(&rproc, 0, sizeof rproc);
	rproc.priv = &ch;

	ALP_ASSERT_EQ_INT(uio_rproc_notify(&rproc, 0x1234u), -1);

	/* #735 requires the bounds check to run BEFORE any shared state is
	 * touched -- neither the scratch word nor the doorbell register may
	 * have been written. */
	uint32_t scratch_word;
	memcpy(&scratch_word,
	       shm_backing + ALP_MHU_SHM_CH1_OFFSET + ALP_MHU_SHM_MSG_TXD,
	       sizeof scratch_word);
	ALP_ASSERT_EQ_INT((int)scratch_word, 0);

	uint32_t doorbell_word;
	memcpy(&doorbell_word,
	       mhu_backing + ALP_MHU_NS_CH5_KICK_SLOT + ALP_MHU_NS_SLOT_MSG_INT_SET,
	       sizeof doorbell_word);
	ALP_ASSERT_EQ_INT((int)doorbell_word, 0);
}

/* Companion positive-path check: the SAME call site must still accept and
 * fire the doorbell when the kick slot genuinely fits the mapping -- proves
 * the bounds check added for #735 does not simply reject everything, and
 * that (given no injected #745 fault) the scratch write lands before the
 * doorbell write. */
static void test_notify_bounds_accepts_in_mapping(void)
{
	unsetenv("ALP_UIO_MHU_KICK_SLOT");

	uint8_t mhu_backing[0x1000];
	uint8_t shm_backing[0x1000];
	memset(mhu_backing, 0, sizeof mhu_backing);
	memset(shm_backing, 0, sizeof shm_backing);

	struct metal_device mhu_dev;
	struct metal_device shm_dev;
	fake_region_init(&mhu_dev, mhu_backing, sizeof mhu_backing);
	fake_region_init(&shm_dev, shm_backing, sizeof shm_backing);

	struct rpc_be ch;
	memset(&ch, 0, sizeof ch);
	ch.dev[UIO_MHU_SHM] = &shm_dev;
	ch.dev[UIO_MHU]     = &mhu_dev;

	struct remoteproc rproc;
	memset(&rproc, 0, sizeof rproc);
	rproc.priv = &ch;

	ALP_ASSERT_EQ_INT(uio_rproc_notify(&rproc, 0xABCDu), 0);

	uint32_t scratch_word;
	memcpy(&scratch_word,
	       shm_backing + ALP_MHU_SHM_CH1_OFFSET + ALP_MHU_SHM_MSG_TXD,
	       sizeof scratch_word);
	ALP_ASSERT_EQ_INT((int)scratch_word, (int)0xABCDu);

	uint32_t doorbell_word;
	memcpy(&doorbell_word,
	       mhu_backing + ALP_MHU_NS_CH5_KICK_SLOT + ALP_MHU_NS_SLOT_MSG_INT_SET,
	       sizeof doorbell_word);
	ALP_ASSERT_EQ_INT((int)doorbell_word, 1);
}

/* ------------------------------------------------------------------ */

int main(void)
{
	memset(&g_fake_rdev, 0, sizeof g_fake_rdev);
	g_fake_rdev.ops.send_offchannel_raw = fake_send_offchannel_raw;

	atomic_store(&g_worker_stop, 0);
	ALP_ASSERT_EQ_INT(pthread_create(&g_worker, NULL, fake_irq_worker, NULL), 0);

	test_self_close_no_uaf_no_selfjoin();
	test_external_close_without_callback_is_synchronous();
	test_concurrent_external_vs_self_close_is_single_shot();
	test_call_vs_close_no_uaf();

	test_mhu_kick_slot_default();
	test_mhu_kick_slot_valid_override();
	test_mhu_kick_slot_rejects_unaligned();
	test_mhu_kick_slot_rejects_u32_truncation();
	test_mhu_kick_slot_rejects_ulong_overflow();
	test_mhu_kick_slot_rejects_garbage();
	test_notify_bounds_reject_out_of_mapping();
	test_notify_bounds_accepts_in_mapping();

	pthread_mutex_lock(&g_mailbox_lock);
	atomic_store(&g_worker_stop, 1);
	pthread_cond_signal(&g_mailbox_cond);
	pthread_mutex_unlock(&g_mailbox_lock);
	pthread_join(g_worker, NULL);

	ALP_TEST_SUMMARY();
}
