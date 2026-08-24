/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Concurrency regression test for issue #1634: alp_adc_stream_close()
 * must drain an in-flight counted op before recycling the handle's pool
 * slot, instead of racing it -- see #629 (the parent open/op/close guard
 * rollout) and #1114 (why the close drain must sleep-poll, not
 * busy-spin).
 *
 * SCOPE NOTE -- what this test does and does not cover:
 *   alp_adc_stream_open()/_read_mv() only reach their GD32-bridge path
 *   (via_bridge == true) on the V2N/V2N-M1 family, through the real
 *   src/zephyr/v2n_supervisor.c singleton and chips/gd32g553/gd32g553.c
 *   driver.  There is no software-fallback streaming-ADC backend and no
 *   simulated GD32 bridge under native_sim, so alp_adc_stream_open()
 *   ALWAYS returns NULL (ALP_ERR_NOSUPPORT / ALP_ERR_NOT_READY) here --
 *   confirmed by this suite's own sibling tests/zephyr/peripheral/src/
 *   adc.c ("Default native_sim builds have no streaming backend").  A
 *   live handle from the public API is therefore unreachable offline,
 *   so alp_adc_stream_read_mv()'s own bridge-blocking window cannot be
 *   driven end-to-end without real V2N/V2N-M1 hardware.
 *
 *   What CAN be proven offline, and what this test proves: this file
 *   manufactures a struct alp_adc_stream directly via the internal pool
 *   (alp_z_adc_stream_pool_acquire(), the same allocator
 *   alp_adc_stream_open() itself uses) with via_bridge=false, then races
 *   a synthetic op -- which counts itself in via the REAL
 *   alp_handle_op_enter()/alp_handle_op_leave() primitives from
 *   src/common/alp_slot_claim.h, the exact call alp_adc_stream_read_mv()
 *   makes at the top of its own body -- against the REAL, unmodified
 *   alp_adc_stream_close().  This exercises the actual fixed code (the
 *   struct's lifecycle/active_ops fields, the pool in
 *   src/zephyr/handles.c, and alp_adc_stream_close()'s
 *   alp_handle_begin_close_blocking() call) end to end; only the "op"
 *   itself (normally alp_adc_stream_read_mv()'s bridge round-trip) is a
 *   test double, standing in for a call this environment cannot make
 *   live.  alp_adc_stream_read_mv()/alp_adc_filter_read_mv()/
 *   alp_adc_spectrum_read_bins() calling alp_handle_op_enter() BEFORE
 *   touching any handle state is verified by inspection of the diff
 *   (op_enter is the first state-touching statement in each), not
 *   re-proven by this test.
 *
 * Deterministic interleave (not a natural-scheduling-race gamble -- see
 * the alp-lab:writing-race-safe-dispatch-handlers skill and issue
 * #1114's own warning that a single-barrier race test can pass with the
 * bug present):
 *
 *   1. Manufacture handle H (channel_id=3).  Pool capacity is 1
 *      (CONFIG_ALP_SDK_MAX_ADC_STREAM_HANDLES=1), so a reopen after H's
 *      close is *guaranteed* to hand back H's exact struct address.
 *   2. Thread READER calls alp_handle_op_enter(&H->lifecycle,
 *      &H->active_ops) (counts the op in, exactly as
 *      alp_adc_stream_read_mv() does), signals `reader_at_gate`, then
 *      blocks on `reader_may_proceed`.
 *   3. Once the main thread observes `reader_at_gate` (READER is
 *      DEFINITELY parked, not just "probably" after a sleep), it starts
 *      thread CLOSER: alp_adc_stream_close(H) followed immediately by a
 *      reopen (channel_id=9) via the same internal pool.
 *        - Pre-fix (bare `in_use` check, no drain): close() tears the
 *          slot down immediately and CLOSER's reopen (same address, pool
 *          capacity 1) overwrites H's fields with channel_id=9 *before*
 *          READER is ever released -- CLOSER finishes almost instantly.
 *        - Post-fix: close() blocks in alp_handle_begin_close_blocking()
 *          because READER's op_enter() already counted this op in, so
 *          CLOSER cannot finish until READER leaves.
 *   4. The test gives CLOSER a short, bounded window to prove it did NOT
 *      already finish, then releases READER.  READER wakes up and reads
 *      H->channel_id -- if CLOSER's reopen already landed, that reads 9
 *      (thread C's data delivered into thread A's read, exactly as the
 *      issue describes); if it hasn't, it reads 3 (READER's own,
 *      correct data).
 *
 * Pre-fix: CLOSER finishes before READER is released, and READER reads
 * channel_id=9 (cross-handle corruption).
 * Post-fix: CLOSER is still blocked when READER is released, and READER
 * reads channel_id=3 (its own handle, untouched).
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <alp/adc.h>
#include <alp/peripheral.h>

#include "alp_slot_claim.h"
#include "handles.h"

/* ---- fixture -------------------------------------------------------------- */

static struct alp_adc_stream *g_h; /* handle under test */

static struct k_sem reader_at_gate;     /* READER signals: op_enter succeeded, parked */
static struct k_sem reader_may_proceed; /* main releases READER */

#define WORKER_STACK_SIZE 2048

static K_THREAD_STACK_DEFINE(reader_stack, WORKER_STACK_SIZE);
static struct k_thread reader_thread_h;
static struct k_sem    reader_done;
static bool            g_op_entered;
static uint32_t        g_captured_channel_id;

static K_THREAD_STACK_DEFINE(closer_stack, WORKER_STACK_SIZE);
static struct k_thread        closer_thread_h;
static struct k_sem           closer_done;
static struct alp_adc_stream *g_reopened;

static void reader_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	/* Exactly what alp_adc_stream_read_mv() does before touching any
	 * handle state -- see the file header's scope note. */
	g_op_entered = alp_handle_op_enter(&g_h->lifecycle, &g_h->active_ops);
	k_sem_give(&reader_at_gate);
	if (g_op_entered) {
		k_sem_take(&reader_may_proceed, K_FOREVER);
		/* Issue #1634's exact vulnerable dereference: read handle
		 * state after a long-blocked op resumes. */
		g_captured_channel_id = g_h->channel_id;
		alp_handle_op_leave(&g_h->active_ops);
	}
	k_sem_give(&reader_done);
}

static void closer_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	alp_adc_stream_close((alp_adc_stream_t *)g_h);

	/* Reopen via the same internal pool alp_adc_stream_open() itself
	 * uses -- pool capacity 1, so this is GUARANTEED to hand back the
	 * exact struct address g_h (and READER's local pointer) still
	 * refers to. */
	struct alp_adc_stream *h2 = alp_z_adc_stream_pool_acquire();
	if (h2 != NULL) {
		h2->via_bridge     = false;
		h2->channel        = 9u;
		h2->channel_id     = 9u;
		h2->sample_rate_hz = 5555u;
		alp_lifecycle_set(&h2->lifecycle, ALP_HANDLE_LC_OPEN);
	}
	g_reopened = h2;
	k_sem_give(&closer_done);
}

ZTEST(alp_adc_stream_close_race, test_close_drains_in_flight_op_before_recycling_slot)
{
	k_sem_init(&reader_at_gate, 0, 1);
	k_sem_init(&reader_may_proceed, 0, 1);
	k_sem_init(&reader_done, 0, 1);
	k_sem_init(&closer_done, 0, 1);
	g_op_entered          = false;
	g_captured_channel_id = 0xFFFFFFFFu; /* sentinel */
	g_reopened            = NULL;

	g_h = alp_z_adc_stream_pool_acquire();
	zassert_not_null(g_h, "alp_z_adc_stream_pool_acquire() failed");
	g_h->via_bridge     = false;
	g_h->channel        = 3u;
	g_h->channel_id     = 3u;
	g_h->sample_rate_hz = 1000u;
	alp_lifecycle_set(&g_h->lifecycle, ALP_HANDLE_LC_OPEN);

	k_tid_t reader_tid = k_thread_create(&reader_thread_h,
	                                     reader_stack,
	                                     K_THREAD_STACK_SIZEOF(reader_stack),
	                                     reader_entry,
	                                     NULL,
	                                     NULL,
	                                     NULL,
	                                     K_PRIO_PREEMPT(5),
	                                     0,
	                                     K_NO_WAIT);

	/* Wait for READER to signal it has entered the op (or failed to) --
	 * deterministic, not a "sleep and hope" delay. */
	zassert_equal(k_sem_take(&reader_at_gate, K_MSEC(1000)), 0, "READER never reached the gate");
	zassert_true(g_op_entered, "alp_handle_op_enter() refused a freshly-opened handle");

	k_tid_t closer_tid = k_thread_create(&closer_thread_h,
	                                     closer_stack,
	                                     K_THREAD_STACK_SIZEOF(closer_stack),
	                                     closer_entry,
	                                     NULL,
	                                     NULL,
	                                     NULL,
	                                     K_PRIO_PREEMPT(5),
	                                     0,
	                                     K_NO_WAIT);

	/* Bounded window: with the fix, CLOSER is blocked in
	 * alp_handle_begin_close_blocking() (active_ops == 1, held by
	 * READER's op_enter()) and genuinely cannot finish here -- this
	 * MUST time out.  Without the fix, close() has no drain guard and
	 * finishes almost immediately. */
	bool closer_finished_before_release = (k_sem_take(&closer_done, K_MSEC(200)) == 0);

	/* Release READER.  It now reads g_h->channel_id -- issue #1634's
	 * exact vulnerable dereference. */
	k_sem_give(&reader_may_proceed);

	zassert_equal(k_thread_join(reader_tid, K_MSEC(2000)), 0, "READER thread never finished");
	if (!closer_finished_before_release) {
		zassert_equal(k_sem_take(&closer_done, K_MSEC(2000)), 0, "CLOSER thread never finished");
	}
	zassert_equal(
	    k_thread_join(closer_tid, K_MSEC(2000)), 0, "CLOSER thread never finished joining");

	zassert_not_null(g_reopened, "CLOSER's reopen failed");
	zassert_equal_ptr(g_reopened,
	                  g_h,
	                  "reopen after close() did not reuse the same struct address -- pool "
	                  "capacity fixture is broken");

	/* The proof (see the file header): CLOSER must not be able to
	 * complete alp_adc_stream_close()+reopen() while READER's counted
	 * op is still in flight. */
	zassert_false(closer_finished_before_release,
	              "alp_adc_stream_close() returned (and the slot was reopened) before the "
	              "in-flight op left -- close() raced the op instead of draining it "
	              "(issue #1634)");
	zassert_equal(g_captured_channel_id,
	              3u,
	              "READER observed channel_id=%u (want 3, its own) after being unblocked -- "
	              "close() recycled the slot to a new owner (channel_id=9) while this op was "
	              "still in flight (issue #1634)",
	              (unsigned)g_captured_channel_id);
}

static void reset_before(void *fixture)
{
	ARG_UNUSED(fixture);
}

ZTEST_SUITE(alp_adc_stream_close_race, NULL, NULL, reset_before, NULL, NULL);
