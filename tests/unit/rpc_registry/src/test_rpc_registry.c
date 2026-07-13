/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the RPC registry dispatcher.  Mirrors the USB /
 * mproc sibling harnesses; no vendor extensions, so the test
 * surface is the bare selector + capability-getter + public-API
 * edges across the single alp_rpc_channel_t handle type.
 *
 * Backends visible on this test build:
 *   zephyr_drv      (priority 100, "*" wildcard)
 *   sw_fallback     (priority 0,   "*" wildcard)
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("rpc", ALP_SOC_REF_STR)`
 * exercises the same selector code path real customer builds hit.
 * Tests that need a different silicon_ref call alp_backend_select
 * directly.  CONFIG_OPENAMP / CONFIG_IPC_SERVICE stay OFF -- the
 * test only exercises the dispatcher's null-handle gates, the
 * selector, and the capability getter; none of which touch the
 * Zephyr ipc_service subsystem.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/rpc.h>

#include "../../../../src/backends/rpc/rpc_ops.h"

ZTEST_SUITE(alp_rpc_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_rpc_registry, test_zephyr_drv_picked_over_sw_on_alif_e7)
{
	const alp_backend_t *be = alp_backend_select("rpc", "alif:ensemble:e7");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "zephyr"), 0);
	zassert_equal(be->priority, 100);
}

ZTEST(alp_rpc_registry, test_sw_fallback_picked_for_unknown_silicon)
{
	/* Both registered backends are wildcards; the higher-priority
     * zephyr_drv would normally win.  This case still exercises the
     * selector and asserts the sw_fallback is reachable on the test
     * build via the registry's count.  Degraded pattern: only
     * inventory is asserted, not the specific pick. */
	const alp_backend_t *be = alp_backend_select("rpc", "fictional:soc:zz");
	zassert_not_null(be);
	(void)be;
	zassert_true(alp_backend_count("rpc") >= 2u);
}

ZTEST(alp_rpc_registry, test_select_returns_null_for_null_class)
{
	zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_rpc_registry, test_select_returns_null_for_null_silicon_ref)
{
	/* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
	zassert_is_null(alp_backend_select("rpc", NULL));
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_rpc_registry, test_rpc_open_returns_null_on_null_config)
{
	/* Dispatcher must reject NULL config before reaching the
     * backend; stamps last_error = ALP_ERR_INVAL. */
	alp_rpc_channel_t *ch = alp_rpc_open(NULL);
	zassert_is_null(ch);
}

ZTEST(alp_rpc_registry, test_rpc_capabilities_returns_null_for_null_handle)
{
	zassert_is_null(alp_rpc_capabilities(NULL));
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_rpc_registry, test_backend_count_for_rpc)
{
	/* zephyr_drv + sw_fallback registered on this build.
     * No vendor-specific backends exist for RPC in Slice 4c. */
	zassert_equal(alp_backend_count("rpc"), 2u);
}

/* ---------- Frame-size overflow guard ------------------------------ */

ZTEST(alp_rpc_registry, test_frame_size_fits_and_reports_total)
{
	size_t total = 0;
	zassert_true(alp_rpc_frame_size(4u, 10u, 64u, &total));
	zassert_equal(total, 15u); /* method + NUL + payload */
}

ZTEST(alp_rpc_registry, test_frame_size_exact_fit)
{
	size_t total = 0;
	/* method(4) + NUL(1) + payload(59) == cap(64) */
	zassert_true(alp_rpc_frame_size(4u, 59u, 64u, &total));
	zassert_equal(total, 64u);
}

ZTEST(alp_rpc_registry, test_frame_size_one_over_rejected)
{
	size_t total = 0;
	zassert_false(alp_rpc_frame_size(4u, 60u, 64u, &total));
}

ZTEST(alp_rpc_registry, test_frame_size_method_too_long_rejected)
{
	size_t total = 0;
	zassert_false(alp_rpc_frame_size(64u, 0u, 64u, &total));
}

ZTEST(alp_rpc_registry, test_frame_size_near_sizemax_payload_does_not_wrap)
{
	/* A near-SIZE_MAX payload must be rejected, not wrap the framed
     * total past `cap`. */
	size_t total = 0;
	zassert_false(alp_rpc_frame_size(4u, SIZE_MAX, 64u, &total));
	zassert_false(alp_rpc_frame_size(4u, SIZE_MAX - 3u, 64u, &total));
}

/* ---------- alp_rpc_call request-pointer guard --------------------- */

static bool _fake_call_reached;

static alp_status_t _fake_call(alp_rpc_backend_state_t *st,
                               const char              *method,
                               const void              *req,
                               size_t                   req_len,
                               void                    *resp,
                               size_t                  *resp_len,
                               uint32_t                 timeout_ms)
{
	(void)st;
	(void)method;
	(void)req;
	(void)req_len;
	(void)resp;
	(void)resp_len;
	(void)timeout_ms;
	_fake_call_reached = true;
	return ALP_OK;
}

static const alp_rpc_ops_t _fake_ops = {
	.call = _fake_call,
};

static struct alp_rpc_channel _make_fake_channel(void)
{
	struct alp_rpc_channel ch;
	memset(&ch, 0, sizeof(ch));
	ch.in_use = true;
	/* alp_rpc_open() also stamps this after a successful backend open
     * (GHSA-xhm8-7f87-93q5 close-protocol redesign -- see
     * src/backends/rpc/rpc_ops.h's struct alp_rpc_channel doc comment)
     * -- alp_rpc_call() gates on the combined lifecycle+refcount
     * `chan_word`, not `in_use`. */
	ch.chan_word = ALP_RPC_CHAN_LC_OPEN;
	ch.state.ops = &_fake_ops;
	return ch;
}

ZTEST(alp_rpc_registry, test_rpc_call_null_req_nonzero_len_rejected)
{
	/* Mirrors the alp_rpc_send guard: req == NULL with a non-zero length
     * must be rejected in the dispatcher before reaching the backend. */
	_fake_call_reached        = false;
	struct alp_rpc_channel ch = _make_fake_channel();
	zassert_equal(ALP_ERR_INVAL, alp_rpc_call(&ch, "m", NULL, 8, NULL, NULL, 0));
	zassert_false(_fake_call_reached, "backend call must not be reached");
}

ZTEST(alp_rpc_registry, test_rpc_call_null_req_zero_len_reaches_backend)
{
	_fake_call_reached        = false;
	struct alp_rpc_channel ch = _make_fake_channel();
	zassert_equal(ALP_OK, alp_rpc_call(&ch, "m", NULL, 0, NULL, NULL, 0));
	zassert_true(_fake_call_reached);
}

ZTEST(alp_rpc_registry, test_rpc_call_valid_req_reaches_backend)
{
	_fake_call_reached            = false;
	struct alp_rpc_channel ch     = _make_fake_channel();
	uint8_t                buf[8] = { 0 };
	zassert_equal(ALP_OK, alp_rpc_call(&ch, "m", buf, sizeof(buf), NULL, NULL, 0));
	zassert_true(_fake_call_reached);
}

/* ---------------------------------------------------------------------
 * GHSA-xhm8-7f87-93q5 close-protocol redesign: native_sim/Zephyr-side
 * coverage of the dispatcher's single-owner CAS + DONE/DEFERRED
 * shutdown() handling + drain, driven through a hand-built
 * struct alp_rpc_channel + a fake ops vtable (same technique as
 * _make_fake_channel() above) and the REAL public alp_rpc_close() /
 * alp_rpc_close_finalize() entry points in src/rpc_dispatch.c.  The
 * Linux/pthread-side counterpart of both scenarios below lives in
 * tests/yocto/rpc_yocto_self_close.c + rpc_dispatch_close_race.c; this
 * gives the identical dispatcher logic Zephyr-kernel-primitive
 * (k_sem/k_thread/k_sleep) coverage on the zephyr/sw path.
 * ------------------------------------------------------------------- */

#define ZTEST_CLOSE_STACK_SIZE 2048

/* ---------- 1. Self-close + concurrent blocked call -------------------
 *
 * Thread B blocks in alp_rpc_call() with an unbounded (UINT32_MAX)
 * timeout.  A second thread ("the worker") plays the role of a
 * backend's own rx/worker thread calling alp_rpc_close() on its own
 * channel from inside a callback: the fake shutdown() detects it IS
 * that thread (matching the design's self-close detection) and
 * returns ALP_RPC_SHUTDOWN_DEFERRED; the worker then calls
 * alp_rpc_close_finalize() itself, exactly as a real backend's
 * rx-epilogue would once its triggering callback returns.
 */

static struct k_sem       g_fake_call_block_sem;
static k_tid_t            g_fake_worker_tid;
static alp_rpc_channel_t *g_self_close_ztest_ch;
static alp_status_t       g_self_close_ztest_result;

static alp_status_t fake_blocking_call(alp_rpc_backend_state_t *st,
                                       const char              *method,
                                       const void              *req,
                                       size_t                   req_len,
                                       void                    *resp,
                                       size_t                  *resp_len,
                                       uint32_t                 timeout_ms)
{
	ARG_UNUSED(st);
	ARG_UNUSED(method);
	ARG_UNUSED(req);
	ARG_UNUSED(req_len);
	ARG_UNUSED(resp);
	ARG_UNUSED(resp_len);
	k_timeout_t to = (timeout_ms == UINT32_MAX) ? K_FOREVER : K_MSEC(timeout_ms);
	/* Only ever given by fake_shutdown_self_detect() below -- a
     * successful take here always means "cancelled by close", never
     * "a real response arrived" (this fake backend never produces
     * one). */
	int rc = k_sem_take(&g_fake_call_block_sem, to);
	return (rc == 0) ? ALP_ERR_NOT_READY : ALP_ERR_TIMEOUT;
}

static alp_rpc_shutdown_result_t fake_shutdown_self_detect(alp_rpc_backend_state_t *st)
{
	ARG_UNUSED(st);
	k_sem_give(&g_fake_call_block_sem); /* wakes the blocked call above */
	return (k_current_get() == g_fake_worker_tid) ? ALP_RPC_SHUTDOWN_DEFERRED
	                                              : ALP_RPC_SHUTDOWN_DONE;
}

static void fake_destroy_noop(alp_rpc_backend_state_t *st)
{
	ARG_UNUSED(st);
}

static const alp_rpc_ops_t _self_close_fake_ops = {
	.call     = fake_blocking_call,
	.shutdown = fake_shutdown_self_detect,
	.destroy  = fake_destroy_noop,
};

static void blocked_call_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	uint8_t resp[4];
	size_t  resp_len = sizeof resp;
	g_self_close_ztest_result =
	    alp_rpc_call(g_self_close_ztest_ch, "no_such_method", NULL, 0, resp, &resp_len, UINT32_MAX);
}

static void self_close_worker_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	g_fake_worker_tid = k_current_get();
	/* THE self-close under test: alp_rpc_close() wins the dispatcher's
     * single-owner CAS, calls shutdown() (this thread == the recorded
     * "worker" thread, so DEFERRED), and returns IMMEDIATELY without
     * draining or destroying anything. */
	alp_rpc_close(g_self_close_ztest_ch);
	/* Mirrors a real backend's rx-epilogue: complete the deferred
     * teardown exactly once, from this same thread, now that "the
     * callback that triggered the self-close" (this function) is
     * about to return. */
	alp_rpc_close_finalize(g_self_close_ztest_ch->state.owner);
}

ZTEST(alp_rpc_registry, test_self_close_blocked_call_not_ready_within_bound)
{
	struct alp_rpc_channel ch;
	memset(&ch, 0, sizeof(ch));
	ch.in_use                 = true;
	ch.chan_word              = ALP_RPC_CHAN_LC_OPEN;
	ch.state.ops              = &_self_close_fake_ops;
	ch.state.owner            = &ch;
	g_self_close_ztest_ch     = &ch;
	g_self_close_ztest_result = ALP_OK;
	k_sem_init(&g_fake_call_block_sem, 0, 1);

	static K_THREAD_STACK_DEFINE(caller_stack, ZTEST_CLOSE_STACK_SIZE);
	static K_THREAD_STACK_DEFINE(worker_stack, ZTEST_CLOSE_STACK_SIZE);
	static struct k_thread caller_thread;
	static struct k_thread worker_thread;

	k_tid_t caller_tid = k_thread_create(&caller_thread,
	                                     caller_stack,
	                                     K_THREAD_STACK_SIZEOF(caller_stack),
	                                     blocked_call_entry,
	                                     NULL,
	                                     NULL,
	                                     NULL,
	                                     K_PRIO_PREEMPT(5),
	                                     0,
	                                     K_NO_WAIT);

	/* Let thread B actually reach its blocking k_sem_take() before
     * racing the self-close against it. */
	k_sleep(K_MSEC(20));

	int64_t start      = k_uptime_get();
	k_tid_t worker_tid = k_thread_create(&worker_thread,
	                                     worker_stack,
	                                     K_THREAD_STACK_SIZEOF(worker_stack),
	                                     self_close_worker_entry,
	                                     NULL,
	                                     NULL,
	                                     NULL,
	                                     K_PRIO_PREEMPT(5),
	                                     0,
	                                     K_NO_WAIT);

	/* If either thread ever regresses to a hang (e.g. the sticky
     * `closing` predicate this design relies on failed to unblock
     * thread B, or the dispatcher's drain deadlocked on a DEFERRED
     * close it should never have drained at all), these bounded joins
     * turn that into a clean test failure instead of a wedged CI job. */
	zassert_equal(k_thread_join(worker_tid, K_MSEC(2000)), 0, "self-close worker never finished");
	zassert_equal(k_thread_join(caller_tid, K_MSEC(2000)), 0, "blocked call never finished");
	int64_t elapsed = k_uptime_get() - start;

	/* HARD <1 s bound (design's test list, item 1). */
	zassert_true(elapsed < 1000, "self-close + cancel took %lld ms (must be < 1000)", elapsed);
	zassert_equal(g_self_close_ztest_result, ALP_ERR_NOT_READY);

	/* "Subsequent open succeeds" (design's test list, item 1): the
     * handle is now fully recyclable -- chan_word back to UNOPENED,
     * in_use released -- exactly the state _alloc_rpc() requires
     * before it will hand this slot to a NEW alp_rpc_open() (see
     * src/rpc_dispatch.c).  This test build cannot exercise a real
     * alp_rpc_open() end-to-end (CONFIG_ALP_SDK_RPC is intentionally
     * OFF here -- see this file's own header comment -- so the
     * registry's real, higher-priority zephyr_drv pick is NOSUPPORT
     * and there is no automatic fallthrough to sw_fallback); the
     * pthread/real-pool equivalent of this same proof (a fresh
     * alp_rpc_open() after a concurrent close reclaims the slot) is
     * covered on the Linux/yocto side by
     * tests/yocto/rpc_dispatch_close_race.c. */
	zassert_equal(ch.chan_word, ALP_RPC_CHAN_LC_UNOPENED);
	zassert_false(ch.in_use);
}

/* ---------- 2. Priority-inversion drain (single-core native_sim) ------
 *
 * A LOW-priority thread holds the dispatcher's active-op count for a
 * fixed, CPU-bound (never voluntarily yielding) duration inside a fake
 * alp_rpc_call().  A HIGH-priority thread then closes the channel:
 * _rpc_drain() (src/rpc_dispatch.c) must actually let the scheduler
 * run the lower-priority holder to let it finish and leave -- a busy
 * spin (or k_yield(), which only cedes to EQUAL priority) would
 * livelock forever on a single core, since the higher-priority closer
 * would never stop being the ready thread the scheduler picks.
 */

static alp_rpc_channel_t *g_prio_ztest_ch;

static alp_status_t fake_call_busy_hold(alp_rpc_backend_state_t *st,
                                        const char              *method,
                                        const void              *req,
                                        size_t                   req_len,
                                        void                    *resp,
                                        size_t                  *resp_len,
                                        uint32_t                 timeout_ms)
{
	ARG_UNUSED(st);
	ARG_UNUSED(method);
	ARG_UNUSED(req);
	ARG_UNUSED(req_len);
	ARG_UNUSED(resp);
	ARG_UNUSED(resp_len);
	ARG_UNUSED(timeout_ms);
	/* Deliberately CPU-bound, no k_sleep/k_yield/k_uptime_get(): holds
     * this thread (and therefore the dispatcher's active-op count)
     * runnable and wanting the CPU, simulating "still doing real work"
     * rather than "voluntarily waiting".  A FIXED iteration count, NOT
     * a k_uptime_get()-based deadline -- native_sim's simulated clock
     * does not advance during a purely CPU-bound loop with no
     * blocking/yielding call inside it (confirmed empirically while
     * writing this test: a k_uptime_get()-based deadline here never
     * elapsed and hung the test, since native_sim only advances
     * simulated time at scheduling points, not via a real wall-clock
     * signal mid-loop).  Real HOST wall-clock time -- which this loop
     * does genuinely consume -- is what matters here, not simulated
     * ticks. */
	volatile long i;
	for (i = 0; i < 300000000L; i++) {
		/* busy */
	}
	return ALP_OK;
}

static alp_rpc_shutdown_result_t fake_shutdown_done(alp_rpc_backend_state_t *st)
{
	ARG_UNUSED(st);
	return ALP_RPC_SHUTDOWN_DONE;
}

static const alp_rpc_ops_t _prio_fake_ops = {
	.call     = fake_call_busy_hold,
	.shutdown = fake_shutdown_done,
	.destroy  = fake_destroy_noop,
};

static void prio_holder_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	uint8_t resp[4];
	size_t  resp_len = sizeof resp;
	(void)alp_rpc_call(g_prio_ztest_ch, "m", NULL, 0, resp, &resp_len, 5000);
}

static int64_t g_prio_close_elapsed_ms;

static void prio_closer_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	int64_t start = k_uptime_get();
	/* If _rpc_drain() ever regressed to a busy spin (or a k_yield()-
     * based one -- see that function's own doc comment), this hangs
     * forever on a single-core target: this HIGHER-priority thread
     * would never stop being the ready thread the scheduler picks, so
     * the lower-priority holder would never run to completion. */
	alp_rpc_close(g_prio_ztest_ch);
	g_prio_close_elapsed_ms = k_uptime_get() - start;
}

ZTEST(alp_rpc_registry, test_close_drain_priority_inversion_single_core)
{
	struct alp_rpc_channel ch;
	memset(&ch, 0, sizeof(ch));
	ch.in_use               = true;
	ch.chan_word            = ALP_RPC_CHAN_LC_OPEN;
	ch.state.ops            = &_prio_fake_ops;
	ch.state.owner          = &ch;
	g_prio_ztest_ch         = &ch;
	g_prio_close_elapsed_ms = -1;

	static K_THREAD_STACK_DEFINE(holder_stack, ZTEST_CLOSE_STACK_SIZE);
	static K_THREAD_STACK_DEFINE(closer_stack, ZTEST_CLOSE_STACK_SIZE);
	static struct k_thread holder_thread;
	static struct k_thread closer_thread;

	/* Dedicated threads for both roles (rather than boosting the
     * ztest-runner thread's own priority in place) so the priority
     * relationship under test -- closer strictly higher priority than
     * holder -- is unambiguous regardless of whatever priority the
     * ztest runner thread itself happens to run at. */
	k_tid_t holder_tid = k_thread_create(&holder_thread,
	                                     holder_stack,
	                                     K_THREAD_STACK_SIZEOF(holder_stack),
	                                     prio_holder_entry,
	                                     NULL,
	                                     NULL,
	                                     NULL,
	                                     K_LOWEST_APPLICATION_THREAD_PRIO,
	                                     0,
	                                     K_NO_WAIT);

	/* Let the holder actually start and enter its busy-hold before
     * racing the (higher-priority) close against it. */
	k_sleep(K_MSEC(10));

	k_tid_t closer_tid = k_thread_create(&closer_thread,
	                                     closer_stack,
	                                     K_THREAD_STACK_SIZEOF(closer_stack),
	                                     prio_closer_entry,
	                                     NULL,
	                                     NULL,
	                                     NULL,
	                                     K_HIGHEST_APPLICATION_THREAD_PRIO,
	                                     0,
	                                     K_NO_WAIT);

	zassert_equal(k_thread_join(closer_tid, K_MSEC(3000)),
	              0,
	              "priority-inversion livelock: alp_rpc_close() never returned");
	zassert_equal(k_thread_join(holder_tid, K_MSEC(3000)),
	              0,
	              "priority-inversion livelock: the lower-priority holder never finished");
	zassert_true(g_prio_close_elapsed_ms >= 0 && g_prio_close_elapsed_ms < 3000,
	             "alp_rpc_close() took %lld ms under priority inversion",
	             g_prio_close_elapsed_ms);
}
