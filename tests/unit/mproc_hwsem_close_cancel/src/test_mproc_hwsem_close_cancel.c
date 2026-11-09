/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * The real Zephyr mproc backend (src/backends/mproc/zephyr_drv.c): an
 * alp_hwsem_lock() blocked on a semaphore another handle holds must be
 * woken by a concurrent alp_hwsem_close() of its own handle with
 * ALP_ERR_NOT_READY (issue #2977 item 3).  The underlying k_sem is shared
 * by every handle on the id, so the backend waits in short slices and
 * re-checks the closing flag; the other handle's lock must be untouched.
 *
 * Before the fix the lock below (UINT32_MAX = K_FOREVER) never returned
 * and alp_hwsem_close() waited for it forever, so the close runs in its
 * own thread and the test joins it with a bound.
 */
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <alp/mproc.h>

#include "alp_sliced_wait.h"

#define STACK_SZ 2048
static K_THREAD_STACK_DEFINE(waiter_stack, STACK_SZ);
static K_THREAD_STACK_DEFINE(closer_stack, STACK_SZ);
static struct k_thread waiter_thr, closer_thr;

static alp_status_t g_lock_rc;

static void waiter(void *h, void *timeout, void *p3)
{
	ARG_UNUSED(p3);
	g_lock_rc = alp_hwsem_lock(h, (uint32_t)(uintptr_t)timeout);
}

static void closer(void *h, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	alp_hwsem_close(h);
}

static void close_wakes_lock(uint32_t timeout_ms)
{
	alp_hwsem_t *holder = alp_hwsem_open(3);
	alp_hwsem_t *wait_h = alp_hwsem_open(3);
	zassert_not_null(holder);
	zassert_not_null(wait_h);
	zassert_equal(alp_hwsem_lock(holder, 0), ALP_OK);

	g_lock_rc = ALP_ERR_BUSY; /* sentinel */
	k_thread_create(&waiter_thr,
	                waiter_stack,
	                STACK_SZ,
	                waiter,
	                wait_h,
	                (void *)(uintptr_t)timeout_ms,
	                NULL,
	                K_PRIO_PREEMPT(1),
	                0,
	                K_NO_WAIT);
	k_msleep(100); /* let it block on the held semaphore */
	k_thread_create(&closer_thr,
	                closer_stack,
	                STACK_SZ,
	                closer,
	                wait_h,
	                NULL,
	                NULL,
	                K_PRIO_PREEMPT(1),
	                0,
	                K_NO_WAIT);

	int64_t t0 = k_uptime_get();
	zassert_equal(k_thread_join(&closer_thr, K_SECONDS(2)), 0, "close() hung on a blocked lock");
	zassert_equal(k_thread_join(&waiter_thr, K_SECONDS(2)), 0, "blocked lock was not woken");
	zassert_true(k_uptime_get() - t0 < 500, "wake took longer than a couple of slices");
	zassert_equal(g_lock_rc, ALP_ERR_NOT_READY);

	/* The holder is unaffected and the semaphore is still usable. */
	zassert_equal(alp_hwsem_unlock(holder), ALP_OK);
	zassert_equal(alp_hwsem_try_lock(holder), ALP_OK);
	zassert_equal(alp_hwsem_unlock(holder), ALP_OK);
	alp_hwsem_close(holder);
}

ZTEST(mproc_hwsem_close_cancel, test_close_wakes_forever_lock)
{
	close_wakes_lock(UINT32_MAX);
}

ZTEST(mproc_hwsem_close_cancel, test_close_wakes_long_timeout_lock)
{
	close_wakes_lock(30000u);
}

ZTEST(mproc_hwsem_close_cancel, test_timeout_still_reported)
{
	alp_hwsem_t *holder = alp_hwsem_open(4);
	alp_hwsem_t *other  = alp_hwsem_open(4);
	zassert_not_null(holder);
	zassert_not_null(other);
	zassert_equal(alp_hwsem_lock(holder, 0), ALP_OK);
	int64_t t0 = k_uptime_get();
	zassert_equal(alp_hwsem_lock(other, 120), ALP_ERR_TIMEOUT);
	int64_t dt = k_uptime_get() - t0;
	zassert_true(dt >= 120 && dt < 400, "timeout %d ms off the requested 120", (int)dt);
	alp_hwsem_close(other);
	alp_hwsem_close(holder);
}

/* The closing-flag expression runs between the previous slice's deadline
 * check and the next slice's budget computation: let a millisecond pass in it so the budget is exactly spent there.  The wait must
 * then report the previous slice's timeout, never issue a 0 ms call. */
static int g_zero_slices;
static int g_closing_flag;

static int *tick_then_flag(void)
{
	k_busy_wait(1000); /* native_sim: simulated time only moves here */
	return &g_closing_flag;
}

ZTEST(mproc_hwsem_close_cancel, test_exhausted_budget_never_issues_zero_ms_slice)
{
	for (int i = 0; i < 20; i++) {
		int      rc = 0;
		uint32_t slice;
		bool     cancelled = ALP_SLICED_WAIT(tick_then_flag(),
		                                     3u,
		                                     slice,
		                                     rc,
		                                     ({
			                                 if (slice == 0) g_zero_slices++;
			                                 -EAGAIN;
		                                     }),
		                                     rc == -EAGAIN);
		zassert_false(cancelled);
		zassert_equal(rc, -EAGAIN);
	}
	zassert_equal(g_zero_slices, 0, "%d zero-ms slices", g_zero_slices);
}

ZTEST_SUITE(mproc_hwsem_close_cancel, NULL, NULL, NULL, NULL, NULL);
