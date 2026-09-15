/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Concurrency regression test for issue #1634: alp_uart_rx_ringbuf_detach()
 * must drain an in-flight counted alp_uart_rx_ringbuf_pop() before
 * recycling the handle's pool slot, instead of racing it -- the same
 * class of bug tests/zephyr/adc_stream_close_race covers for
 * alp_adc_stream_t, applied to struct alp_uart_rx_ringbuf (the other
 * whole-struct-zero-on-acquire handle in src/zephyr/handles.h that
 * carried a bare `in_use` with no active-op tracking).
 *
 * Drives the REAL public alp_uart_rx_ringbuf_pop()/_detach() entry
 * points (not a hand-rolled alp_handle_op_enter() call) via a
 * test-only synchronisation hook, alp_uart_rx_ringbuf_pop_test_sync_hook
 * -- src/backends/uart/zephyr_drv.c calls it right after op_enter has
 * counted the op in and before the actual lwrb_read(), the same
 * pattern src/backends/rpc/zephyr_drv.c's g_rpc_recv_test_sync_hook
 * uses (this one has external linkage because this test links the
 * real built alp_sdk library rather than #including the backend .c
 * file directly).
 *
 * Deterministic interleave (mirrors adc_stream_close_race's own):
 *
 *   1. Manufacture ring buffer handle H directly via
 *      alp_z_uart_rx_ringbuf_pool_acquire() + lwrb_init(), with dev/port
 *      left NULL (no real UART device needed -- detach() already treats
 *      a NULL dev/port as "nothing to unwire"). Pool capacity is 1
 *      (CONFIG_ALP_SDK_MAX_UART_RX_RINGBUF_HANDLES=1), so a reopen after
 *      H's detach is *guaranteed* to hand back H's exact struct address.
 *      Seed H's ring with 5 known bytes ("MINE!").
 *   2. Thread READER calls the REAL alp_uart_rx_ringbuf_pop(H, ...).
 *      Inside it, right after alp_handle_op_enter() succeeds, the test
 *      hook signals `reader_at_gate` and blocks on `reader_may_proceed`
 *      -- op_enter has counted the op in, but the actual lwrb_read()
 *      has NOT run yet.
 *   3. Once the main thread observes `reader_at_gate`, it starts thread
 *      CLOSER: alp_uart_rx_ringbuf_detach(H) followed immediately by a
 *      reopen (same pool, capacity 1) that re-inits the SAME struct
 *      address with a DIFFERENT backing buffer seeded with different
 *      bytes ("OTHR!").
 *        - Pre-fix (bare `in_use` check, no drain): detach() tears the
 *          slot down immediately (lwrb_free on H's own ring) and
 *          CLOSER's reopen overwrites H's rb field with the "OTHR!"
 *          ring *before* READER is ever released -- CLOSER finishes
 *          almost instantly.
 *        - Post-fix: detach() blocks in alp_handle_begin_close_blocking()
 *          because READER's op_enter() already counted this op in, so
 *          CLOSER cannot finish until READER leaves.
 *   4. The test gives CLOSER a short, bounded window to prove it did NOT
 *      already finish, then releases READER via the hook. READER's
 *      pop() then runs its real lwrb_read() and returns -- if CLOSER's
 *      reopen already landed, that reads "OTHR!" (thread C's data
 *      delivered into thread A's read, exactly as the issue describes);
 *      if it hasn't, it reads "MINE!" (READER's own, correct data).
 *
 * Pre-fix: CLOSER finishes before READER is released, and READER reads
 * "OTHR!" (cross-handle corruption).
 * Post-fix: CLOSER is still blocked when READER is released, and READER
 * reads "MINE!" (its own handle, untouched).
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <alp/peripheral.h>

#include "alp_slot_claim.h"
#include "handles.h"

/* Declared (non-static, external linkage) in src/backends/uart/zephyr_drv.c
 * -- see this file's header comment. */
extern void (*alp_uart_rx_ringbuf_pop_test_sync_hook)(void);

/* ---- fixture -------------------------------------------------------------- */

static struct alp_uart_rx_ringbuf *g_rb; /* handle under test */

static uint8_t g_backing_orig[16];
static uint8_t g_backing_other[16];

static struct k_sem reader_at_gate;     /* READER signals: op_enter succeeded, parked */
static struct k_sem reader_may_proceed; /* main releases READER */

#define WORKER_STACK_SIZE 2048

static K_THREAD_STACK_DEFINE(reader_stack, WORKER_STACK_SIZE);
static struct k_thread reader_thread_h;
static struct k_sem    reader_done;
static alp_status_t    g_read_rc;
static size_t          g_got;
static uint8_t         g_captured[8];

static K_THREAD_STACK_DEFINE(closer_stack, WORKER_STACK_SIZE);
static struct k_thread             closer_thread_h;
static struct k_sem                closer_done;
static struct alp_uart_rx_ringbuf *g_reopened;

static void reader_sync_hook(void)
{
	k_sem_give(&reader_at_gate);
	k_sem_take(&reader_may_proceed, K_FOREVER);
}

static void reader_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	memset(g_captured, 0, sizeof(g_captured));
	g_read_rc = alp_uart_rx_ringbuf_pop(g_rb, g_captured, sizeof(g_captured), &g_got);
	k_sem_give(&reader_done);
}

static void closer_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	alp_uart_rx_ringbuf_detach((alp_uart_rx_ringbuf_t *)g_rb);

	/* Reopen via the same internal pool alp_uart_rx_ringbuf_attach()
	 * itself uses -- pool capacity 1, so this is GUARANTEED to hand
	 * back the exact struct address g_rb (and READER's local pointer)
	 * still refers to. */
	struct alp_uart_rx_ringbuf *h2 = alp_z_uart_rx_ringbuf_pool_acquire();
	if (h2 != NULL) {
		if (lwrb_init(&h2->rb, g_backing_other, sizeof(g_backing_other)) != 0u) {
			(void)lwrb_write(&h2->rb, "OTHR!", 5);
		}
		h2->dev  = NULL;
		h2->port = NULL;
		alp_lifecycle_set(&h2->lifecycle, ALP_HANDLE_LC_OPEN);
	}
	g_reopened = h2;
	k_sem_give(&closer_done);
}

ZTEST(alp_uart_rx_ringbuf_close_race, test_detach_drains_in_flight_pop_before_recycling_slot)
{
	k_sem_init(&reader_at_gate, 0, 1);
	k_sem_init(&reader_may_proceed, 0, 1);
	k_sem_init(&reader_done, 0, 1);
	k_sem_init(&closer_done, 0, 1);
	g_read_rc  = ALP_ERR_NOT_READY;
	g_got      = 0xFFFFu; /* sentinel */
	g_reopened = NULL;

	g_rb = alp_z_uart_rx_ringbuf_pool_acquire();
	zassert_not_null(g_rb, "alp_z_uart_rx_ringbuf_pool_acquire() failed");
	zassert_equal(lwrb_init(&g_rb->rb, g_backing_orig, sizeof(g_backing_orig)),
	              1,
	              "lwrb_init() on the seed handle failed");
	g_rb->dev  = NULL;
	g_rb->port = NULL;
	zassert_equal(lwrb_write(&g_rb->rb, "MINE!", 5), 5, "seeding the ring with MINE! failed");
	alp_lifecycle_set(&g_rb->lifecycle, ALP_HANDLE_LC_OPEN);

	alp_uart_rx_ringbuf_pop_test_sync_hook = reader_sync_hook;

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

	/* Wait for READER to reach the hook (op_enter succeeded, about to
	 * block) -- deterministic, not a "sleep and hope" delay. */
	zassert_equal(k_sem_take(&reader_at_gate, K_MSEC(1000)), 0, "READER never reached the gate");

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
	 * MUST time out.  Without the fix, detach() has no drain guard and
	 * finishes almost immediately. */
	bool closer_finished_before_release = (k_sem_take(&closer_done, K_MSEC(200)) == 0);

	/* Release READER via the hook.  It now runs its real lwrb_read() --
	 * issue #1634's exact vulnerable dereference. */
	k_sem_give(&reader_may_proceed);

	zassert_equal(k_thread_join(reader_tid, K_MSEC(2000)), 0, "READER thread never finished");
	if (!closer_finished_before_release) {
		zassert_equal(k_sem_take(&closer_done, K_MSEC(2000)), 0, "CLOSER thread never finished");
	}
	zassert_equal(
	    k_thread_join(closer_tid, K_MSEC(2000)), 0, "CLOSER thread never finished joining");

	alp_uart_rx_ringbuf_pop_test_sync_hook = NULL;

	zassert_not_null(g_reopened, "CLOSER's reopen failed");
	zassert_equal_ptr(g_reopened,
	                  g_rb,
	                  "reopen after detach() did not reuse the same struct address -- pool "
	                  "capacity fixture is broken");

	/* The proof (see the file header): CLOSER must not be able to
	 * complete alp_uart_rx_ringbuf_detach()+reopen() while READER's
	 * counted op is still in flight. */
	zassert_false(closer_finished_before_release,
	              "alp_uart_rx_ringbuf_detach() returned (and the slot was reopened) before "
	              "the in-flight op left -- detach() raced the op instead of draining it "
	              "(issue #1634)");
	zassert_equal(g_read_rc, ALP_OK, "alp_uart_rx_ringbuf_pop() returned %d", (int)g_read_rc);
	zassert_equal(g_got, 5, "READER got %zu bytes (want 5, its own seed)", g_got);
	zassert_mem_equal(g_captured,
	                  "MINE!",
	                  5,
	                  "READER read cross-handle data after being unblocked -- detach() "
	                  "recycled the slot to a new owner's ring while this pop was still in "
	                  "flight (issue #1634)");
}

ZTEST_SUITE(alp_uart_rx_ringbuf_close_race, NULL, NULL, NULL, NULL, NULL);
