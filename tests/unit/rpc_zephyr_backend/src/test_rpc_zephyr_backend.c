/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * White-box native_sim/ztest coverage for GHSA-xhm8-7f87-93q5 defects
 * 1 and 3 in src/backends/rpc/zephyr_drv.c (defect 2's `closing` +
 * `cb_active` entry/exit gate is exercised as a side effect of both
 * scenarios below, via the shared rpc_recv_enter()/rpc_recv_leave()
 * helpers the fix introduced).
 *
 * This file #includes src/backends/rpc/zephyr_drv.c directly (the
 * same technique tests/yocto/rpc_yocto_self_close.c uses for the
 * Linux backend -- see that file's header comment) so it can reach
 * the exact file-local surface under test: `struct rpc_be`,
 * `rpc_ept_recv()`, `z_shutdown()`, `frame_build()`, `fnv1a_32()`.
 * Real ipc_service (an actual /dev-equivalent RPMsg transport) is
 * bench-only on Zephyr -- there is no way to fake a working
 * ipc_service instance under native_sim without real shared-memory +
 * mailbox devicetree nodes -- so every scenario below drives the
 * backend's internal state directly rather than going through a real
 * z_open()/ipc_service round trip.  z_open() itself is never called
 * here; z_shutdown()'s `ipc_service_deregister_endpoint()` call on an
 * unregistered (zeroed) `struct ipc_ept` is harmless -- Zephyr's own
 * subsys/ipc/ipc_service.c returns -ENOENT for `ept->instance == NULL`
 * without touching anything -- so the external-close path below is
 * safe to exercise without a real endpoint.
 *
 * `alp_rpc_close_finalize()` is normally defined by src/rpc_dispatch.c
 * (not linked here -- see this directory's CMakeLists.txt for why);
 * a minimal test-local double below tracks how many times, and with
 * which owner, it was actually invoked.
 *
 * Three scenarios:
 *
 *   1. test_defect1_recv_vs_timed_out_call_race -- uses
 *      zephyr_drv.c's g_rpc_recv_test_sync_hook (a test-only seam
 *      mirroring src/backends/rpc/yocto_drv.c's
 *      g_y_call_test_late_staging_hook) to wake a HIGHER-priority
 *      "canceller" thread from INSIDE rpc_ept_recv()'s synchronous-
 *      call critical section, right after it has decided
 *      `call_pending` is true and is about to write the response --
 *      the exact instant GHSA-xhm8-7f87-93q5 defect 1 exploited.  The
 *      canceller mirrors z_call()'s real -EAGAIN branch: it first
 *      takes a real (non-blocking) k_sem_take() on `be->call_sem` to
 *      decide whether a response has genuinely already arrived, and
 *      only cancels + reclaims (poisons) the response buffer if that
 *      genuinely times out.  Because the fixed code holds `be->lock`
 *      (a spinlock -- interrupts disabled) across the ENTIRE
 *      check-then-write, the higher-priority canceller cannot actually
 *      run AT ALL until rpc_ept_recv() has finished writing the real
 *      response and given `call_sem` and released the lock -- so by
 *      the time the canceller's k_sem_take() runs, it always sees the
 *      real response and never reaches the cancel/poison branch: the
 *      response buffer ends up with the real payload, never torn,
 *      never poisoned-then-overwritten.  This is a genuine, deliberate
 *      regression test: temporarily removing the lock around the
 *      routing block (verified by hand while writing this test)
 *      reproduces the defect exactly -- the canceller's `call_sem`
 *      wait then genuinely times out (recv hasn't reached the
 *      `k_sem_give` yet), it poisons the buffer, and rpc_ept_recv()
 *      then resumes and overwrites the poison with the real payload --
 *      a write into a buffer the "caller" had already reclaimed.
 *
 *   2. test_defect1_recv_consumes_pending_call_when_uncontested --
 *      positive control for (1): with no concurrent cancel, an
 *      ordinary pending call's response is still routed normally.
 *
 *   3. test_defect3_cross_channel_close_takes_external_path -- channel
 *      B's rpc_ept_recv() runs once (stamping its `recv_thread` to
 *      the CURRENT thread, then clearing `recv_active` at exit --
 *      exactly what a real, unrelated, earlier receive does).  Later,
 *      STILL on the same thread, channel A's rpc_ept_recv() runs a
 *      subscriber callback that calls z_shutdown() on BOTH channel A
 *      (self) and channel B (cross-channel).  Pre-fix (bare
 *      `k_current_get() == recv_thread` comparison), channel B's
 *      stale-but-thread-matching `recv_thread` made z_shutdown(B)
 *      misfire DEFERRED -- wedging B forever, since B's own
 *      rpc_ept_recv() is not on this call stack to ever run the
 *      deferred epilogue.  Post-fix, z_shutdown(B) must return DONE
 *      (external path, `alp_rpc_close_finalize()` must NOT fire for
 *      B) while z_shutdown(A) still correctly returns DEFERRED (the
 *      genuine self-close), and A's own rpc_ept_recv() epilogue must
 *      complete A's deferred teardown -- alp_rpc_close_finalize()
 *      firing exactly once, with owner == &g_chan_a_state.
 */

/* Faked purely at the preprocessor level -- no real Kconfig ALP_SDK_RPC
 * symbol exists in this image (see this directory's CMakeLists.txt for
 * why CONFIG_ALP_SDK is deliberately never set here).  Toggles ONLY
 * the code-path guard inside zephyr_drv.c; every ipc_service_ /
 * device_is_ready symbol it references still resolves against the
 * REAL Zephyr CONFIG_IPC_SERVICE core subsystem this app's prj.conf
 * enables for real (subsys/ipc/ipc_service.c, unconditional on which
 * backend is selected -- ipc_service_register_endpoint()/
 * deregister_endpoint()/open_instance()/send() live there, not in a
 * backend-specific TU).  CONFIG_OPENAMP/CONFIG_IPC_SERVICE_BACKEND_RPMSG
 * themselves are ALSO faked here rather than set via real Kconfig:
 * CONFIG_OPENAMP depends on the `open-amp` west module being present
 * in THIS workspace's manifest (it is not, in every environment this
 * runs in), and CONFIG_IPC_SERVICE_BACKEND_RPMSG depends on a real
 * MBOX device/instance this test never touches (z_open() -- the only
 * place a real ipc_dev/endpoint would matter -- is never called by
 * any test below).  Both macros exist here ONLY to satisfy
 * zephyr_drv.c's own #error guard; neither gates any symbol this test
 * actually needs to link against. */
#define CONFIG_ALP_SDK_RPC               1
#define CONFIG_OPENAMP                   1
#define CONFIG_IPC_SERVICE_BACKEND_RPMSG 1

#include "../../../../src/backends/rpc/zephyr_drv.c"

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

ZTEST_SUITE(alp_rpc_zephyr_backend, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------ */
/* Test double for the dispatcher's alp_rpc_close_finalize()           */
/* ------------------------------------------------------------------ */

static atomic_t g_finalize_calls;
static void    *g_finalize_owner;

void alp_rpc_close_finalize(void *owner)
{
	g_finalize_owner = owner;
	atomic_inc(&g_finalize_calls);
}

/* ------------------------------------------------------------------ */
/* Shared helpers                                                      */
/* ------------------------------------------------------------------ */

/* Minimal, valid `struct rpc_be` -- mirrors z_open()'s field init
 * (name/pool bookkeeping) without the real ipc_service dance z_open()
 * itself performs; `ept`/`ept_cfg` stay zeroed (no real endpoint), so
 * z_shutdown()'s ipc_service_deregister_endpoint() call is a documented
 * no-op (see this file's header comment). */
static void init_test_channel(struct rpc_be *be, const char *name)
{
	memset(be, 0, sizeof(*be));
	strncpy(be->name, name, sizeof(be->name) - 1);
	k_sem_init(&be->call_sem, 0, 1);
	k_mutex_init(&be->tx_mutex);
	be->in_use = true;
}

/* ------------------------------------------------------------------ */
/* 1. GHSA-xhm8-7f87-93q5 defect 1: recv-vs-timed-out-call race.        */
/* ------------------------------------------------------------------ */

#define RACE_STACK_SIZE 2048
#define RACE_RESP_CAP   8
#define RACE_BOUND_MS   2000

static const uint8_t RACE_PAYLOAD_BYTE = 0xAB;
static const uint8_t RACE_POISON_BYTE  = 0xDE;

struct race_ctx {
	struct rpc_be *be;
	uint8_t        resp_buf[RACE_RESP_CAP];
};

static struct race_ctx g_race_ctx;
static struct k_sem    g_canceller_woken;
static atomic_t        g_canceller_ran;
static atomic_t        g_canceller_saw_timeout;
static struct k_thread g_canceller_thread;
static K_THREAD_STACK_DEFINE(g_canceller_stack, RACE_STACK_SIZE);

/* g_rpc_recv_test_sync_hook (declared in zephyr_drv.c, right inside
 * rpc_ept_recv()'s synchronous-call check, before it writes the
 * response): wakes a HIGHER-priority canceller thread.  Non-blocking
 * (a single k_sem_give()) -- see this file's header comment and
 * zephyr_drv.c's own doc comment on the hook for why it must never
 * block. */
static void recv_write_race_hook(void)
{
	k_sem_give(&g_canceller_woken);
}

static void canceller_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	struct rpc_be *be = g_race_ctx.be;

	k_sem_take(&g_canceller_woken, K_FOREVER);
	atomic_set(&g_canceller_ran, 1);

	/* Mirrors z_call()'s own decision: a REAL, non-blocking check of
	 * whether the response already arrived. Fixed code: rpc_ept_recv()
	 * holds `be->lock` (a spinlock -- disables scheduling) across its
	 * entire check-then-write, so THIS thread (however high its
	 * priority) cannot even run until that critical section --
	 * including its own k_sem_give(&be->call_sem) -- has fully
	 * completed; by the time this line runs, the real response has
	 * therefore ALWAYS already arrived. */
	int rc = k_sem_take(&be->call_sem, K_NO_WAIT);
	if (rc != 0) {
		/* Genuine timeout (only reachable pre-fix): cancel under the
		 * SAME lock rpc_ept_recv() takes, then reclaim (poison) the
		 * response buffer -- mirrors z_call()'s -EAGAIN branch and the
		 * caller's stack frame going out of scope. */
		atomic_set(&g_canceller_saw_timeout, 1);
		k_spinlock_key_t key = k_spin_lock(&be->lock);
		be->call_pending     = false;
		be->call_result      = ALP_ERR_NOT_READY;
		k_spin_unlock(&be->lock, key);
		memset(g_race_ctx.resp_buf, RACE_POISON_BYTE, RACE_RESP_CAP);
	}
}

static void race_recv_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	uint8_t payload[RACE_RESP_CAP];
	memset(payload, RACE_PAYLOAD_BYTE, sizeof(payload));

	uint8_t frame[64];
	int     built = frame_build(frame, sizeof(frame), "probe", payload, sizeof(payload));
	zassert_true(built > 0, "frame_build failed");

	rpc_ept_recv(frame, (size_t)built, g_race_ctx.be);
}

ZTEST(alp_rpc_zephyr_backend, test_defect1_recv_vs_timed_out_call_race)
{
	static K_THREAD_STACK_DEFINE(recv_stack, RACE_STACK_SIZE);
	static struct k_thread recv_thread_h;

	struct rpc_be be;

	k_sem_init(&g_canceller_woken, 0, 1);
	atomic_clear(&g_canceller_ran);
	atomic_clear(&g_canceller_saw_timeout);

	init_test_channel(&be, "race");
	g_race_ctx.be = &be;

	/* Stage the call slot exactly as z_call() does under `be->lock`. */
	k_spinlock_key_t key = k_spin_lock(&be.lock);
	strncpy(be.call_method, "probe", sizeof(be.call_method) - 1);
	be.call_method[sizeof(be.call_method) - 1] = '\0';
	be.call_resp_buf                           = g_race_ctx.resp_buf;
	be.call_resp_cap                           = sizeof(g_race_ctx.resp_buf);
	be.call_resp_len                           = 0u;
	be.call_result                             = ALP_ERR_TIMEOUT;
	be.call_pending                            = true;
	k_spin_unlock(&be.lock, key);

	g_rpc_recv_test_sync_hook = recv_write_race_hook;

	/* HIGHER priority than recv below: once woken (inside recv's
	 * critical section, pre-fix; only after it, post-fix), it always
	 * gets the CPU before recv's own next instruction. */
	k_thread_create(&g_canceller_thread,
	                g_canceller_stack,
	                K_THREAD_STACK_SIZEOF(g_canceller_stack),
	                canceller_entry,
	                NULL,
	                NULL,
	                NULL,
	                K_PRIO_PREEMPT(4),
	                0,
	                K_NO_WAIT);

	k_tid_t recv_tid = k_thread_create(&recv_thread_h,
	                                   recv_stack,
	                                   K_THREAD_STACK_SIZEOF(recv_stack),
	                                   race_recv_entry,
	                                   NULL,
	                                   NULL,
	                                   NULL,
	                                   K_PRIO_PREEMPT(5),
	                                   0,
	                                   K_NO_WAIT);

	zassert_equal(k_thread_join(recv_tid, K_MSEC(RACE_BOUND_MS)), 0, "recv thread never finished");
	g_rpc_recv_test_sync_hook = NULL;

	/* Bounded wait for the (higher-priority, so normally already done)
	 * canceller to fully finish before inspecting shared state. */
	int waited_ms = 0;
	while (!atomic_get(&g_canceller_ran) && waited_ms < RACE_BOUND_MS) {
		k_sleep(K_MSEC(1));
		waited_ms += 1;
	}
	zassert_true(atomic_get(&g_canceller_ran), "canceller thread never ran");

	uint8_t expect_payload[RACE_RESP_CAP];
	memset(expect_payload, RACE_PAYLOAD_BYTE, sizeof(expect_payload));

	/* The decisive assertion: the canceller ran at the earliest
	 * possible instant (right when recv decided to write), yet the
	 * lock means it can only ever observe the response as ALREADY
	 * delivered -- never a genuine timeout, never a poisoned-then-
	 * overwritten buffer. */
	zassert_false(atomic_get(&g_canceller_saw_timeout),
	              "canceller saw a timeout despite recv having decided to respond -- "
	              "the lock did not serialise them");
	zassert_mem_equal(g_race_ctx.resp_buf, expect_payload, RACE_RESP_CAP);
	zassert_equal(be.call_result, ALP_OK);
	zassert_false(be.call_pending);
}

/* Positive control: with NO concurrent cancel, rpc_ept_recv() must
 * still route a genuinely pending call's response normally -- the
 * fix must not have made the ordinary, uncontested path stop
 * working. */
ZTEST(alp_rpc_zephyr_backend, test_defect1_recv_consumes_pending_call_when_uncontested)
{
	struct rpc_be   be;
	struct race_ctx ctx;

	init_test_channel(&be, "uncontested");
	ctx.be                    = &be;
	g_rpc_recv_test_sync_hook = NULL;

	k_spinlock_key_t key = k_spin_lock(&be.lock);
	strncpy(be.call_method, "probe", sizeof(be.call_method) - 1);
	be.call_method[sizeof(be.call_method) - 1] = '\0';
	be.call_resp_buf                           = ctx.resp_buf;
	be.call_resp_cap                           = sizeof(ctx.resp_buf);
	be.call_resp_len                           = 0u;
	be.call_result                             = ALP_ERR_TIMEOUT;
	be.call_pending                            = true;
	k_spin_unlock(&be.lock, key);

	uint8_t payload[RACE_RESP_CAP];
	memset(payload, RACE_PAYLOAD_BYTE, sizeof(payload));
	uint8_t frame[64];
	int     built = frame_build(frame, sizeof(frame), "probe", payload, sizeof(payload));
	zassert_true(built > 0);

	rpc_ept_recv(frame, (size_t)built, &be);

	zassert_mem_equal(ctx.resp_buf, payload, RACE_RESP_CAP);
	zassert_equal(be.call_result, ALP_OK);
	zassert_false(be.call_pending);
}

/* ------------------------------------------------------------------ */
/* 2. GHSA-xhm8-7f87-93q5 defect 3: cross-channel self-close.           */
/* ------------------------------------------------------------------ */

static struct rpc_be           g_chan_a;
static struct rpc_be           g_chan_b;
static alp_rpc_backend_state_t g_chan_a_state;
static alp_rpc_backend_state_t g_chan_b_state;

static alp_rpc_shutdown_result_t g_result_a;
static alp_rpc_shutdown_result_t g_result_b;

static void on_close_other(const void *payload, size_t len, void *user)
{
	ARG_UNUSED(payload);
	ARG_UNUSED(len);
	ARG_UNUSED(user);

	/* Cross-channel close: channel A's own callback closes channel B.
	 * B's rpc_ept_recv() is NOT on this call stack -- only A's is --
	 * so this must take the external DONE path, never DEFERRED. */
	g_result_b = z_shutdown(&g_chan_b_state);

	/* Self-close: channel A's own callback closes channel A -- this
	 * IS the genuine self-close, must return DEFERRED. */
	g_result_a = z_shutdown(&g_chan_a_state);
}

ZTEST(alp_rpc_zephyr_backend, test_defect3_cross_channel_close_takes_external_path)
{
	atomic_clear(&g_finalize_calls);
	g_finalize_owner = NULL;
	g_result_a       = (alp_rpc_shutdown_result_t)-1;
	g_result_b       = (alp_rpc_shutdown_result_t)-1;

	init_test_channel(&g_chan_a, "chan_a");
	init_test_channel(&g_chan_b, "chan_b");
	g_chan_a_state.be_data = &g_chan_a;
	g_chan_b_state.be_data = &g_chan_b;
	g_chan_a_state.owner   = &g_chan_a_state;
	g_chan_b_state.owner   = &g_chan_b_state;
	g_chan_a.owner         = &g_chan_a_state;
	g_chan_b.owner         = &g_chan_b_state;

	/* Channel B receives one, unrelated, earlier frame on THIS same
	 * thread -- stamps b.recv_thread to k_current_get() and leaves it
	 * stale (recv_active cleared back to false at exit) exactly like a
	 * real, completed receive would.  This is the stale state defect 3
	 * exploited. */
	{
		uint8_t frame[64];
		int     built = frame_build(frame, sizeof(frame), "unrelated", NULL, 0);
		zassert_true(built > 0);
		rpc_ept_recv(frame, (size_t)built, &g_chan_b);
	}
	zassert_false(g_chan_b.recv_active, "chan B must not still be 'in recv' after it returned");
	zassert_equal(g_chan_b.recv_thread,
	              k_current_get(),
	              "test setup requires chan B's stale recv_thread to match the current thread");

	/* Channel A subscribes "close_other" -> on_close_other() above. */
	g_chan_a.subs[0].method_hash = fnv1a_32("close_other");
	strncpy(g_chan_a.subs[0].method, "close_other", sizeof(g_chan_a.subs[0].method) - 1);
	g_chan_a.subs[0].cb   = on_close_other;
	g_chan_a.subs[0].user = NULL;

	/* Fire the frame that drives channel A's rpc_ept_recv() into the
	 * subscriber callback above -- STILL on this same thread, so
	 * chan_b.recv_thread's stale match is live. */
	{
		uint8_t frame[64];
		int     built = frame_build(frame, sizeof(frame), "close_other", NULL, 0);
		zassert_true(built > 0);
		rpc_ept_recv(frame, (size_t)built, &g_chan_a);
	}

	/* The decisive assertions (GHSA-xhm8-7f87-93q5 defect 3): */
	zassert_equal(g_result_b,
	              ALP_RPC_SHUTDOWN_DONE,
	              "cross-channel close must take the external DONE path, not DEFERRED");
	zassert_equal(
	    g_result_a, ALP_RPC_SHUTDOWN_DEFERRED, "genuine self-close must still return DEFERRED");

	/* Channel A's rpc_ept_recv() epilogue must have completed the
	 * deferred teardown exactly once, for channel A -- never for B. */
	zassert_equal(
	    atomic_get(&g_finalize_calls), 1, "alp_rpc_close_finalize() must fire exactly once");
	zassert_equal(g_finalize_owner,
	              &g_chan_a_state,
	              "alp_rpc_close_finalize() must fire for channel A, not B");

	/* Channel B must be structurally untouched by the misfired close:
	 * still open from this backend's point of view (closing was never
	 * even a signal here -- z_shutdown(B) ran the plain external path,
	 * which this test's fake ept makes a fast no-op/wait, but B's
	 * `closing` sticky flag IS still set by z_shutdown(B) itself,
	 * matching a REAL external close's contract). */
	zassert_true(g_chan_b.closing, "external z_shutdown(B) must still set the sticky closing flag");
}
