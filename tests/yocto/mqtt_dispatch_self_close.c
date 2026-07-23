/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #756: alp_mqtt_loop() (src/mqtt_dispatch.c)
 * invokes the subscribed message callback SYNCHRONOUSLY, inline, from
 * inside the backend's ops->loop() call, before returning.  A callback
 * that called alp_mqtt_close() on its OWN handle used to deadlock:
 * alp_mqtt_close()'s sleep-poll drain waits for active_ops to reach 0,
 * but that count is THIS very loop() call, on THIS very thread, which
 * cannot decrement until the callback returns -- and it cannot return
 * while asleep inside close().  This affects BOTH the Yocto Mosquitto
 * and Zephyr MQTT backends identically, since the bug lives entirely
 * above the ops vtable, in the dispatcher itself.
 *
 * This file #includes src/mqtt_dispatch.c directly (rather than
 * linking the built library) so it can drive the REAL, fixed
 * alp_mqtt_loop()/alp_mqtt_close() entry points against a minimal FAKE
 * backend (registered by hand, bypassing alp_backend_select() --
 * neither sw_fallback nor a real broker ever invokes a message
 * callback synchronously, so a fake ops->loop() is the only way to
 * reproduce the self-close deterministically without a live broker).
 *
 * Three scenarios:
 *   1. test_self_close_from_loop_callback -- the issue reproduction: a
 *      message callback closes its own handle from inside
 *      alp_mqtt_loop().  Single-threaded and deterministic: if this
 *      regresses to the pre-#756 shape the process hangs (see this
 *      file's CTest TIMEOUT) rather than returning.
 *   2. test_close_from_other_thread_while_loop_blocks -- an external
 *      close from a DIFFERENT thread while alp_mqtt_loop() is still
 *      in flight elsewhere must still block-and-drain as before (not
 *      regress to always-deferred).
 *   3. test_close_from_other_thread_during_callback_vs_slot_reuse --
 *      dev-review follow-up: an external close on ANOTHER thread while
 *      alp_mqtt_loop() is mid-callback (cb_active/cb_thread set, but no
 *      self-close requested), racing a THIRD thread that continuously
 *      recycles the (single-slot, see CONFIG_ALP_SDK_MAX_MQTT_HANDLES
 *      below) handle pool.  The pre-fix ordering read h->close_pending
 *      AFTER alp_handle_op_leave() -- the exact release that lets the
 *      external closer's drain complete and free/recycle the slot --
 *      so that read could land on a slot a third open() had already
 *      memset() over (or, if that reopened slot's own callback
 *      self-closes in the same window, cause THIS thread to run
 *      teardown a second time on a live handle).  Run under
 *      ThreadSanitizer (see alp_test_can_gpio_tsan in this directory's
 *      CMakeLists.txt) to prove no such race remains.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_mqtt_dispatch_self_close
 *   ctest --test-dir build -R alp_test_mqtt_dispatch_self_close
 */

#include <pthread.h>
#include <stdatomic.h>
#include <time.h>

#include "test_assert.h"

/* Force a single-slot pool (see scenario 3's own comment): guarantees
 * a third thread's alp_mqtt_open()-equivalent recycle always lands on
 * the SAME slot the racing handle occupies, rather than a 50/50 shot
 * at a second free slot. */
#define CONFIG_ALP_SDK_MAX_MQTT_HANDLES 1

#include "../../src/mqtt_dispatch.c"

/* ------------------------------------------------------------------ */
/* 1. A message callback closes its OWN handle from inside loop().     */
/* ------------------------------------------------------------------ */

static atomic_int  g_cb_entered;
static atomic_int  g_cb_returned;
static alp_mqtt_t *g_self_close_handle;

static void self_close_msg_cb(const char *topic, const uint8_t *payload, size_t len, void *user)
{
	(void)topic;
	(void)payload;
	(void)len;
	(void)user;
	atomic_store(&g_cb_entered, 1);
	/* THE self-close under test. */
	alp_mqtt_close(g_self_close_handle);
	atomic_store(&g_cb_returned, 1);
}

static alp_status_t fake_loop_self_close(alp_mqtt_backend_state_t *state, uint32_t timeout_ms)
{
	(void)state;
	(void)timeout_ms;
	self_close_msg_cb(NULL, NULL, 0, NULL);
	return ALP_OK;
}

static void fake_close_noop(alp_mqtt_backend_state_t *state)
{
	(void)state;
}

static const alp_mqtt_ops_t fake_ops_self_close = {
	.loop  = fake_loop_self_close,
	.close = fake_close_noop,
};

static void test_self_close_from_loop_callback(void)
{
	atomic_store(&g_cb_entered, 0);
	atomic_store(&g_cb_returned, 0);

	struct alp_mqtt *h = _alloc_mqtt();
	ALP_ASSERT_TRUE(h != NULL);
	h->state.ops = &fake_ops_self_close;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	g_self_close_handle = h;

	/* Pre-#756 this hangs forever (alp_mqtt_close()'s drain spins on
     * this very thread's own in-flight loop() count); this file's
     * CTest entry sets a bounded TIMEOUT so a regression fails CI
     * instead of wedging it. */
	alp_status_t rc = alp_mqtt_loop(h, 1000);
	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_TRUE(atomic_load(&g_cb_entered));
	ALP_ASSERT_TRUE(atomic_load(&g_cb_returned));

	/* The handle must be FULLY torn down (not left dangling mid-close):
     * lifecycle back to UNOPENED and the slot released -- prove the
     * latter by re-claiming it and getting the SAME slot back. */
	ALP_ASSERT_EQ_INT(alp_lifecycle_get(&h->lifecycle), ALP_HANDLE_LC_UNOPENED);
	struct alp_mqtt *reclaimed = _alloc_mqtt();
	ALP_ASSERT_TRUE(reclaimed != NULL);
	_free_mqtt(reclaimed);
}

/* ------------------------------------------------------------------ */
/* 2. External close from a DIFFERENT thread while loop() is blocked   */
/*    elsewhere must still block-and-drain (unchanged behaviour).      */
/* ------------------------------------------------------------------ */

static atomic_int  g_loop_entered;
static atomic_int  g_loop_left;
static alp_mqtt_t *g_ext_close_handle;

static alp_status_t fake_loop_blocks(alp_mqtt_backend_state_t *state, uint32_t timeout_ms)
{
	(void)state;
	(void)timeout_ms;
	atomic_store(&g_loop_entered, 1);
	/* Simulate a real broker round-trip: block for a bit so the
     * external close below has to genuinely wait (drain), not race
     * past an instant return. */
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L }; /* 100 ms */
	nanosleep(&ts, NULL);
	atomic_store(&g_loop_left, 1);
	return ALP_OK;
}

static const alp_mqtt_ops_t fake_ops_blocks = {
	.loop  = fake_loop_blocks,
	.close = fake_close_noop,
};

static void *loop_thread(void *arg)
{
	(void)arg;
	(void)alp_mqtt_loop(g_ext_close_handle, 1000);
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

static void test_close_from_other_thread_while_loop_blocks(void)
{
	atomic_store(&g_loop_entered, 0);
	atomic_store(&g_loop_left, 0);

	struct alp_mqtt *h = _alloc_mqtt();
	ALP_ASSERT_TRUE(h != NULL);
	h->state.ops = &fake_ops_blocks;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	g_ext_close_handle = h;

	pthread_t t;
	ALP_ASSERT_EQ_INT(pthread_create(&t, NULL, loop_thread, NULL), 0);
	ALP_ASSERT_TRUE(wait_until(&g_loop_entered, 1000));

	/* External close (this, the main thread, is NOT the thread inside
     * loop()) -- must block until loop() actually leaves, not tear the
     * handle down out from under it. */
	alp_mqtt_close(h);
	ALP_ASSERT_TRUE(atomic_load(&g_loop_left));
	ALP_ASSERT_EQ_INT(alp_lifecycle_get(&h->lifecycle), ALP_HANDLE_LC_UNOPENED);

	ALP_ASSERT_EQ_INT(pthread_join(t, NULL), 0);
}

/* ------------------------------------------------------------------ */
/* 3. External close on another thread DURING the callback, racing a   */
/*    third thread recycling the (single-slot) handle pool.            */
/* ------------------------------------------------------------------ */

static atomic_int  g_reuse_loop_entered;
static atomic_int  g_reuse_loop_left;
static alp_mqtt_t *g_reuse_handle;
static atomic_int  g_reuse_stop;
static atomic_int  g_reuse_teardown_count;

static alp_status_t fake_loop_slow_no_selfclose(alp_mqtt_backend_state_t *state,
                                                uint32_t                  timeout_ms)
{
	(void)state;
	(void)timeout_ms;
	atomic_store(&g_reuse_loop_entered, 1);
	/* Long enough for the racing close() below to reliably observe
     * cb_active==true and enter its own drain before this returns. */
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 20000000L }; /* 20 ms */
	nanosleep(&ts, NULL);
	atomic_store(&g_reuse_loop_left, 1);
	return ALP_OK;
}

static void fake_close_count_reuse(alp_mqtt_backend_state_t *state)
{
	(void)state;
	atomic_fetch_add(&g_reuse_teardown_count, 1);
}

static const alp_mqtt_ops_t fake_ops_slow_no_selfclose = {
	.loop  = fake_loop_slow_no_selfclose,
	.close = fake_close_count_reuse,
};

static void *loop_thread_reuse(void *arg)
{
	(void)arg;
	(void)alp_mqtt_loop(g_reuse_handle, 1000);
	return NULL;
}

static void *close_thread_reuse(void *arg)
{
	(void)arg;
	/* External close: this thread is NOT the one inside loop(), so
     * alp_handle_begin_close_selfaware() always takes the NOW path here
     * regardless of timing -- see this file's header comment for the
     * exact race this proves closed. */
	(void)wait_until(&g_reuse_loop_entered, 1000);
	alp_mqtt_close(g_reuse_handle);
	return NULL;
}

/* Continuously claims + releases the (single-slot) pool -- see
 * CONFIG_ALP_SDK_MAX_MQTT_HANDLES above -- so that the instant
 * close_thread_reuse()'s teardown releases g_reuse_handle's slot, some
 * OTHER "open" immediately memset()s over it, maximising the chance of
 * colliding with a stale post-op_leave read of that same memory (the
 * pre-fix ordering) under ThreadSanitizer. */
/* Simulates an unrelated session that opens the just-recycled slot and
 * immediately self-closes it -- sets close_pending true (and restores
 * a non-NULL ops pointer over _alloc_mqtt()'s memset, so the guard in
 * the buggy post-op_leave path doesn't mask the effect) exactly the
 * way a real self-close would, without needing the full open/loop
 * dance (irrelevant here -- only the fields matter for the ordering
 * bug this proves).  The pre-fix ordering could then read THIS
 * unrelated close_pending as if it were g_reuse_handle's own,
 * double-closing/double-freeing the live handle -- see
 * g_reuse_teardown_count below. */
static atomic_int g_recycle_claims;
static atomic_int g_recycle_freeze_request;
static atomic_int g_recycle_frozen;

/* Claims + releases the (single-slot) pool, setting an unrelated
 * sentinel close_pending=true each time (see this section's own
 * comment above) -- EXCEPT once widen_after_op_leave() below requests
 * a freeze, in which case this thread claims ONE more slot, sets the
 * sentinel, and then stops WITHOUT releasing it: leaves
 * close_pending==true pinned in place instead of immediately looping
 * back around to memset() it away again, so the read this scenario is
 * proving safe has a guaranteed (not merely probable) window to land
 * in. */
static void *recycle_thread(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_reuse_stop)) {
		struct alp_mqtt *h2 = _alloc_mqtt();
		if (h2 != NULL) {
			h2->state.ops = &fake_ops_slow_no_selfclose;
			__atomic_store_n(&h2->close_pending, true, __ATOMIC_RELEASE);
			if (atomic_load(&g_recycle_freeze_request)) {
				atomic_store(&g_recycle_frozen, 1);
				break;
			}
			_free_mqtt(h2);
			atomic_fetch_add(&g_recycle_claims, 1);
		}
	}
	return NULL;
}

/* Widens the window right after alp_handle_op_leave() (see
 * g_mqtt_test_after_op_leave_hook's doc comment in mqtt_dispatch.c):
 * the external closer's sleep-poll drain reacts on a 1ms cadence, far
 * coarser than the few instructions this thread takes to reach its own
 * next line, so without this the recycle_thread/close_thread_reuse
 * race below would need scheduler luck to land in a regression's
 * vulnerable window.  Waits for a few ordinary claim/set/free cycles
 * (proving recycling actually happens at all), THEN requests
 * recycle_thread pin close_pending==true in place (rather than merely
 * hoping to sample it mid-cycle -- the skill's "make the interleaving
 * deterministic" guidance) before returning.  Harmless on the fixed
 * code: nothing after alp_handle_op_leave() touches `h` in the
 * not-self-closed path this scenario exercises.  Both waits capped so
 * a genuinely broken recycle_thread fails this test instead of
 * hanging it. */
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

static void test_close_from_other_thread_during_callback_vs_slot_reuse(void)
{
	atomic_store(&g_reuse_loop_entered, 0);
	atomic_store(&g_reuse_loop_left, 0);
	atomic_store(&g_reuse_stop, 0);
	atomic_store(&g_reuse_teardown_count, 0);
	atomic_store(&g_recycle_claims, 0);
	atomic_store(&g_recycle_freeze_request, 0);
	atomic_store(&g_recycle_frozen, 0);
	g_mqtt_test_after_op_leave_hook = widen_after_op_leave;

	struct alp_mqtt *h = _alloc_mqtt();
	ALP_ASSERT_TRUE(h != NULL);
	h->state.ops = &fake_ops_slow_no_selfclose;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	g_reuse_handle = h;

	pthread_t recycle_t[4], loop_t, close_t;
	for (size_t i = 0; i < 4; ++i) {
		ALP_ASSERT_EQ_INT(pthread_create(&recycle_t[i], NULL, recycle_thread, NULL), 0);
	}
	ALP_ASSERT_EQ_INT(pthread_create(&loop_t, NULL, loop_thread_reuse, NULL), 0);
	ALP_ASSERT_EQ_INT(pthread_create(&close_t, NULL, close_thread_reuse, NULL), 0);

	ALP_ASSERT_EQ_INT(pthread_join(loop_t, NULL), 0);
	ALP_ASSERT_EQ_INT(pthread_join(close_t, NULL), 0);
	atomic_store(&g_reuse_stop, 1);
	for (size_t i = 0; i < 4; ++i) {
		ALP_ASSERT_EQ_INT(pthread_join(recycle_t[i], NULL), 0);
	}
	g_mqtt_test_after_op_leave_hook = NULL;

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
	test_self_close_from_loop_callback();
	test_close_from_other_thread_while_loop_blocks();
	test_close_from_other_thread_during_callback_vs_slot_reuse();
	ALP_TEST_SUMMARY();
}
