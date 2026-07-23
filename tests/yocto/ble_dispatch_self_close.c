/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #756: alp_ble_scan_start()
 * (src/ble_dispatch.c) may invoke the scan callback SYNCHRONOUSLY,
 * inline, once per result, from inside the backend's ops->scan_start()
 * call, before returning (the CC3501E backend does exactly this: it
 * runs the scan to completion, then fans results out to the callback
 * -- see src/backends/ble/cc3501e.c's cc35_scan_start()).  A callback
 * that called alp_ble_close() on its own radio handle used to
 * deadlock the same way alp_mqtt_loop()'s message callback did:
 * alp_ble_close()'s sleep-poll drain waits for active_ops to reach 0,
 * but that count is THIS very scan_start() call, on THIS very thread,
 * which cannot decrement until the callback returns -- and it cannot
 * return while asleep inside close().
 *
 * This file #includes src/ble_dispatch.c directly (rather than linking
 * the built library) so it can drive the REAL, fixed
 * alp_ble_scan_start()/alp_ble_close() entry points against a minimal
 * FAKE backend (registered by hand, bypassing alp_backend_select()) --
 * neither sw_fallback nor a real controller invokes a scan callback
 * synchronously in a way this test can trigger deterministically
 * without live RF, so a fake ops->scan_start() reproduces the
 * self-close deterministically instead.
 *
 * Three scenarios:
 *   1. test_self_close_from_scan_callback -- the issue reproduction: a
 *      scan-result callback closes its own radio handle from inside
 *      alp_ble_scan_start().  Single-threaded and deterministic: if
 *      this regresses to the pre-#756 shape the process hangs (see
 *      this file's CTest TIMEOUT) rather than returning.
 *   2. test_close_from_other_thread_while_scan_blocks -- an external
 *      close from a DIFFERENT thread while alp_ble_scan_start() is
 *      still in flight elsewhere must still block-and-drain as before.
 *   3. test_close_from_other_thread_during_scan_vs_slot_reuse --
 *      dev-review follow-up: an external close on ANOTHER thread while
 *      alp_ble_scan_start() is mid-callback-window (cb_active/cb_thread
 *      set, no self-close requested), racing a THIRD thread that
 *      continuously recycles the (single-slot, see
 *      CONFIG_ALP_SDK_MAX_BLE_HANDLES below) radio pool.  Mirrors
 *      tests/yocto/mqtt_dispatch_self_close.c's identical scenario 3 --
 *      see that file's own comment for the exact race this closes.  Run
 *      under ThreadSanitizer (see alp_test_can_gpio_tsan in this
 *      directory's CMakeLists.txt).
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_ble_dispatch_self_close
 *   ctest --test-dir build -R alp_test_ble_dispatch_self_close
 */

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "test_assert.h"

/* Force a single-slot radio pool (see scenario 3's own comment). */
#define CONFIG_ALP_SDK_MAX_BLE_HANDLES 1

#include "../../src/ble_dispatch.c"

/* ------------------------------------------------------------------ */
/* 1. A scan callback closes its OWN radio handle from inside          */
/*    scan_start().                                                    */
/* ------------------------------------------------------------------ */

static atomic_int g_cb_entered;
static atomic_int g_cb_returned;
static alp_ble_t *g_self_close_handle;

static void self_close_scan_cb(const alp_ble_scan_result_t *r, void *user)
{
	(void)r;
	(void)user;
	atomic_store(&g_cb_entered, 1);
	/* THE self-close under test. */
	alp_ble_close(g_self_close_handle);
	atomic_store(&g_cb_returned, 1);
}

static alp_status_t fake_scan_start_self_close(alp_ble_radio_state_t *state,
                                               bool                   active,
                                               alp_ble_scan_cb_t      cb,
                                               void                  *user)
{
	(void)state;
	(void)active;
	alp_ble_scan_result_t r;
	memset(&r, 0, sizeof(r));
	cb(&r, user); /* mirrors cc35_scan_start()'s synchronous fan-out */
	return ALP_OK;
}

static void fake_close_noop(alp_ble_radio_state_t *state)
{
	(void)state;
}

static const alp_ble_ops_t fake_ops_self_close = {
	.scan_start = fake_scan_start_self_close,
	.close      = fake_close_noop,
};

static void test_self_close_from_scan_callback(void)
{
	atomic_store(&g_cb_entered, 0);
	atomic_store(&g_cb_returned, 0);

	struct alp_ble *h = _alloc_radio();
	ALP_ASSERT_TRUE(h != NULL);
	h->state.ops = &fake_ops_self_close;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	g_self_close_handle = h;

	/* Pre-#756 this hangs forever (alp_ble_close()'s drain spins on
     * this very thread's own in-flight scan_start() count); this
     * file's CTest entry sets a bounded TIMEOUT so a regression fails
     * CI instead of wedging it. */
	alp_status_t rc = alp_ble_scan_start(h, true, self_close_scan_cb, NULL);
	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_TRUE(atomic_load(&g_cb_entered));
	ALP_ASSERT_TRUE(atomic_load(&g_cb_returned));

	ALP_ASSERT_EQ_INT(alp_lifecycle_get(&h->lifecycle), ALP_HANDLE_LC_UNOPENED);
	struct alp_ble *reclaimed = _alloc_radio();
	ALP_ASSERT_TRUE(reclaimed != NULL);
	_free_radio(reclaimed);
}

/* ------------------------------------------------------------------ */
/* 2. External close from a DIFFERENT thread while scan_start() is     */
/*    blocked elsewhere must still block-and-drain (unchanged).        */
/* ------------------------------------------------------------------ */

static atomic_int g_scan_entered;
static atomic_int g_scan_left;
static alp_ble_t *g_ext_close_handle;

static void noop_scan_cb(const alp_ble_scan_result_t *r, void *user)
{
	(void)r;
	(void)user;
}

static alp_status_t
fake_scan_start_blocks(alp_ble_radio_state_t *state, bool active, alp_ble_scan_cb_t cb, void *user)
{
	(void)state;
	(void)active;
	atomic_store(&g_scan_entered, 1);
	/* Simulate a real scan window: block for a bit so the external
     * close below has to genuinely wait (drain), not race past an
     * instant return. */
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L }; /* 100 ms */
	nanosleep(&ts, NULL);
	atomic_store(&g_scan_left, 1);
	alp_ble_scan_result_t r;
	memset(&r, 0, sizeof(r));
	cb(&r, user);
	return ALP_OK;
}

static const alp_ble_ops_t fake_ops_blocks = {
	.scan_start = fake_scan_start_blocks,
	.close      = fake_close_noop,
};

static void *scan_thread(void *arg)
{
	(void)arg;
	(void)alp_ble_scan_start(g_ext_close_handle, true, noop_scan_cb, NULL);
	return NULL;
}

static bool wait_until(atomic_int *flag, int timeout_ms)
{
	int waited_ms = 0;
	while (!atomic_load(flag)) {
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L };
		nanosleep(&ts, NULL);
		if (++waited_ms >= timeout_ms) return false;
	}
	return true;
}

static void test_close_from_other_thread_while_scan_blocks(void)
{
	atomic_store(&g_scan_entered, 0);
	atomic_store(&g_scan_left, 0);

	struct alp_ble *h = _alloc_radio();
	ALP_ASSERT_TRUE(h != NULL);
	h->state.ops = &fake_ops_blocks;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	g_ext_close_handle = h;

	pthread_t t;
	ALP_ASSERT_EQ_INT(pthread_create(&t, NULL, scan_thread, NULL), 0);
	ALP_ASSERT_TRUE(wait_until(&g_scan_entered, 1000));

	/* External close (this, the main thread, is NOT the thread inside
     * scan_start()) -- must block until scan_start() actually leaves. */
	alp_ble_close(h);
	ALP_ASSERT_TRUE(atomic_load(&g_scan_left));
	ALP_ASSERT_EQ_INT(alp_lifecycle_get(&h->lifecycle), ALP_HANDLE_LC_UNOPENED);

	ALP_ASSERT_EQ_INT(pthread_join(t, NULL), 0);
}

/* ------------------------------------------------------------------ */
/* 3. External close on another thread DURING scan_start(), racing a   */
/*    third thread recycling the (single-slot) radio pool.             */
/* ------------------------------------------------------------------ */

static alp_ble_t *g_reuse_handle;
static atomic_int g_reuse_stop;
static atomic_int g_reuse_teardown_count;

static void fake_close_count_reuse(alp_ble_radio_state_t *state)
{
	(void)state;
	atomic_fetch_add(&g_reuse_teardown_count, 1);
}

static const alp_ble_ops_t fake_ops_reuse = {
	.scan_start = fake_scan_start_blocks,
	.close      = fake_close_count_reuse,
};

static void *scan_thread_reuse(void *arg)
{
	(void)arg;
	/* cb_active/cb_thread are set around the WHOLE scan_start() call by
     * the dispatcher wrapper, so any op that blocks for a while (with
     * or without itself invoking the callback) sets up the exact
     * window this scenario races -- see this file's header comment. */
	(void)alp_ble_scan_start(g_reuse_handle, true, noop_scan_cb, NULL);
	return NULL;
}

static void *close_thread_reuse(void *arg)
{
	(void)arg;
	/* External close: this thread is NOT the one inside scan_start(),
     * so alp_handle_begin_close_selfaware() always takes the NOW path
     * here regardless of timing. */
	(void)wait_until(&g_scan_entered, 1000);
	alp_ble_close(g_reuse_handle);
	return NULL;
}

static atomic_int g_recycle_claims;
static atomic_int g_recycle_freeze_request;
static atomic_int g_recycle_frozen;

/* Claims + releases the (single-slot) pool -- see
 * CONFIG_ALP_SDK_MAX_BLE_HANDLES above -- setting an unrelated
 * sentinel close_pending=true each cycle, mirrors
 * tests/yocto/mqtt_dispatch_self_close.c's identical recycle_thread
 * (see that file's doc comment for the full rationale, incl. why this
 * pins the sentinel in place once widen_after_op_leave() requests a
 * freeze instead of always looping straight back around). */
static void *recycle_thread(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_reuse_stop)) {
		struct alp_ble *h2 = _alloc_radio();
		if (h2 != NULL) {
			h2->state.ops = &fake_ops_reuse;
			__atomic_store_n(&h2->close_pending, true, __ATOMIC_RELEASE);
			if (atomic_load(&g_recycle_freeze_request)) {
				atomic_store(&g_recycle_frozen, 1);
				break;
			}
			_free_radio(h2);
			atomic_fetch_add(&g_recycle_claims, 1);
		}
	}
	return NULL;
}

/* Widens the window right after alp_handle_op_leave() -- mirrors
 * tests/yocto/mqtt_dispatch_self_close.c's identical
 * widen_after_op_leave(); see that file's doc comment for the full
 * rationale (deterministic instead of scheduler-luck-dependent). */
static void widen_after_op_leave(void)
{
	int start     = atomic_load(&g_recycle_claims);
	int waited_ms = 0;
	while (atomic_load(&g_recycle_claims) < start + 10 && waited_ms < 2000) {
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L }; /* 1 ms */
		nanosleep(&ts, NULL);
		++waited_ms;
	}
	atomic_store(&g_recycle_freeze_request, 1);
	waited_ms = 0;
	while (!atomic_load(&g_recycle_frozen) && waited_ms < 2000) {
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L }; /* 1 ms */
		nanosleep(&ts, NULL);
		++waited_ms;
	}
}

static void test_close_from_other_thread_during_scan_vs_slot_reuse(void)
{
	atomic_store(&g_scan_entered, 0);
	atomic_store(&g_scan_left, 0);
	atomic_store(&g_reuse_stop, 0);
	atomic_store(&g_reuse_teardown_count, 0);
	atomic_store(&g_recycle_claims, 0);
	atomic_store(&g_recycle_freeze_request, 0);
	atomic_store(&g_recycle_frozen, 0);
	g_ble_test_after_op_leave_hook = widen_after_op_leave;

	struct alp_ble *h = _alloc_radio();
	ALP_ASSERT_TRUE(h != NULL);
	h->state.ops = &fake_ops_reuse;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	g_reuse_handle = h;

	pthread_t recycle_t[4], scan_t, close_t;
	for (size_t i = 0; i < 4; ++i) {
		ALP_ASSERT_EQ_INT(pthread_create(&recycle_t[i], NULL, recycle_thread, NULL), 0);
	}
	ALP_ASSERT_EQ_INT(pthread_create(&scan_t, NULL, scan_thread_reuse, NULL), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&close_t, NULL, close_thread_reuse, NULL), 0);

	ALP_ASSERT_EQ_INT(pthread_join(scan_t, NULL), 0);
	ALP_ASSERT_EQ_INT(pthread_join(close_t, NULL), 0);
	atomic_store(&g_reuse_stop, 1);
	for (size_t i = 0; i < 4; ++i) {
		ALP_ASSERT_EQ_INT(pthread_join(recycle_t[i], NULL), 0);
	}
	g_ble_test_after_op_leave_hook = NULL;

	/* THE assertion: exactly ONE teardown of g_reuse_handle -- the
     * external close_thread_reuse() above.  A regression to the
     * pre-fix ordering can read a stray close_pending==true left by
     * recycle_thread()'s unrelated sentinel session and run a SECOND,
     * spurious teardown (double-close/double-free) on the same
     * handle. */
	ALP_ASSERT_EQ_INT(atomic_load(&g_reuse_teardown_count), 1);
}

int main(void)
{
	test_self_close_from_scan_callback();
	test_close_from_other_thread_while_scan_blocks();
	test_close_from_other_thread_during_scan_vs_slot_reuse();
	ALP_TEST_SUMMARY();
}
