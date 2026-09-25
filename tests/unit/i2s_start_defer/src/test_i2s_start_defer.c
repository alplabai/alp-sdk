/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression test for issue #2132: alp_i2s_start() called before the
 * first alp_i2s_write() must not fail on the Alif DesignWare I2S
 * driver's empty TX ring buffer -- and the fix (deferred start, live in
 * src/backends/i2s/zephyr_drv.c) must not silently swallow a START that
 * keeps failing, must not strand queued blocks on stop(), must not
 * leave a stale block for the next open() to dequeue into freed memory,
 * and (review round 2) must not report a chunk as queued when a failed
 * retry DROPped it (MAJOR-1), and must not leave tx_pending_start stuck
 * true past a successful start() (MAJOR-2, tx_started => !tx_pending_
 * start enforced by construction -- see _mark_tx_started()'s comment in
 * the backend); and (review round 4) a concurrent write()-vs-stop() race
 * must not resurrect the original -ENOMEM symptom via a stale
 * tx_block_queued -- reproduced single-threaded via
 * fake_i2s_set_write_hook(), see test_start_write_stop_race_stale_flag_
 * recovers below.
 *
 * issue #2137 (a separate, independent bug once a stream starts and
 * then goes quiet): a TX underrun -- the queue running dry while
 * RUNNING -- drops i2s_dw into I2S_STATE_ERROR (i2s_tx_irq_handler()'s
 * queue-empty branch), and from there DRAIN, START, and write() all
 * -EIO forever (i2s_dw_trigger()'s DRAIN/START cases, i2s_dw_write()):
 * nothing in the pre-#2137 backend ever issued the ONE trigger valid
 * from ERROR (I2S_TRIGGER_PREPARE) or the DROP fallback that also works
 * there. z_stop()/z_start()/z_write() now recover
 * transparently -- see test_underrun_then_stop_recovers,
 * test_underrun_then_start_and_write_replays,
 * test_underrun_then_write_resumes_with_no_slab_leak, and
 * test_underrun_write_prepare_refused_propagates_original_error below.
 *
 * issue #2137 review round 2 closed five more gaps found against that
 * first pass: the RX-side leak in i2s_dw.c's own overrun ISR path
 * (fixed at its actual source, not in this backend -- see
 * test_rx_overrun_recovers_no_slab_leak), a stale tx_started surviving
 * the NOMEM-as-stale-flag branch and breaking the tx_started =>
 * !tx_pending_start invariant (test_underrun_start_retry_clears_
 * started_invariant), z_stop() attempting a DRAIN it already knows will
 * be refused on a genuinely-idle TX stream (silent before, now provable
 * via fake_i2s_drain_call_count()), z_write() giving up on a refused
 * PREPARE instead of still retrying (test_underrun_write_retry_fails_
 * after_prepare_succeeds_no_leak proves the RETRY's own failure
 * surfaces correctly instead), and z_start()'s own refused-PREPARE case
 * having no direct test (test_underrun_start_prepare_refused_
 * propagates_original_error).
 *
 * Round 2's OWN first-pass fix for the refused-PREPARE-retry case
 * introduced a NEW bug rather than closing one: it reset the TX flags
 * UNCONDITIONALLY whenever this branch was entered, not only when THIS
 * CALL's own PREPARE succeeded. Review round 3 reproduced this on real
 * API interleavings (fake_i2s_set_write_fail_hook() -- see below) rather
 * than the round-2 test's own knob, which cleared I2S_STATE_ERROR
 * directly with no trigger and no flag update, a state no real caller
 * could ever produce, and so could not catch it: see
 * test_ilv_a_second_writer_after_concurrent_writer_recovery,
 * test_ilv_b_writer_does_not_undo_concurrent_stop, and
 * test_ilv_d_writer_resumes_after_concurrent_start_recovery. The fix
 * reverts to resetting only on THIS call's own successful PREPARE --
 * see z_write()'s own comment for why that is correct: every state-
 * changing trigger updates the tx_* flags under the same lock it uses,
 * so a concurrent call that already left ERROR has already set them
 * correctly by the time this call's own PREPARE observes the refusal.
 *
 * fake_i2s.c models the real i2s_dw.c state machine closely enough to
 * catch all of the above: unlike tests/unit/i2s_write_bounds' always-
 * succeeds fake (which never calls trigger(START) at all), this one
 * refuses an empty-queue START (-ENOMEM), refuses STOP/DRAIN on a non-
 * running stream (-EIO), and only DROP releases queued blocks
 * unconditionally -- exactly the three-way split z_start()/z_stop()/
 * z_close() now route through. Its forced-failure knob
 * (fake_i2s_force_start_fail()) pins THIS BACKEND's retry/DROP logic,
 * not i2s_dw fidelity -- see fake_i2s.c's own header comment for why a
 * forced failure on an otherwise-healthy READY-with-something-queued
 * START cannot happen on real i2s_dw.
 *
 * Every test captures each call's result to a local and closes its
 * handle BEFORE any zassert_*: CONFIG_ALP_SDK_MAX_I2S_HANDLES defaults
 * to 2, so a failing assertion that skipped the close would leak a pool
 * slot and cascade into later tests' open_tx() -- mirrors
 * tests/zephyr/chips/src/test_audio.c's close-before-assert convention
 * (see tas_init()'s comment there). Measured: an earlier draft of this
 * file that asserted inline caught the real #2132 defect fine on the
 * FIRST such test, but then cascaded "h is NULL" failures through
 * every test after it once the leaked handle exhausted the pool --
 * masking which assertions were real regressions.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/i2s.h>

#include "fake_i2s.h"

ZTEST_SUITE(alp_i2s_start_defer, NULL, NULL, NULL, NULL, NULL);

#define BLOCK_FRAMES 32u
#define BLOCK_BYTES  (BLOCK_FRAMES * 1u * 2u) /* mono S16: frames * channels * (bits/8) */

static alp_i2s_t *open_tx(void)
{
	alp_i2s_config_t cfg = {
		.bus_id         = 0u,
		.sample_rate_hz = 16000u,
		.word_bits      = 16u,
		.channels       = 1u,
		.format         = ALP_I2S_FMT_I2S,
		.direction      = ALP_I2S_DIR_TX,
		.block_frames   = BLOCK_FRAMES,
	};
	return alp_i2s_open(&cfg);
}

static alp_i2s_t *open_rx(void)
{
	alp_i2s_config_t cfg = {
		.bus_id         = 0u,
		.sample_rate_hz = 16000u,
		.word_bits      = 16u,
		.channels       = 1u,
		.format         = ALP_I2S_FMT_I2S,
		.direction      = ALP_I2S_DIR_RX,
		.block_frames   = BLOCK_FRAMES,
	};
	return alp_i2s_open(&cfg);
}

static alp_status_t write_block(alp_i2s_t *h)
{
	static uint8_t block[BLOCK_BYTES];
	memset(block, 0xAA, sizeof(block));
	return alp_i2s_write(h, block, sizeof(block), 100u);
}

/* Core regression: start() before the first write() must return ALP_OK
 * immediately (the real trigger is deferred), and the write that
 * follows must be the call that actually fires it. */
ZTEST(alp_i2s_start_defer, test_direct_start_then_write_succeeds)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();

	/* Pre-fix: this hit i2s_dw.c's empty-queue -ENOMEM immediately --
	 * issue #2132. */
	alp_status_t start_rc             = alp_i2s_start(h);
	bool         running_before_write = fake_i2s_tx_running();
	alp_status_t write_rc             = write_block(h);
	bool         running_after_write  = fake_i2s_tx_running();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(start_rc, ALP_OK, "deferred start must not fail");
	zassert_false(running_before_write, "start() with nothing queued must not trigger yet");
	zassert_equal(write_rc, ALP_OK);
	zassert_true(running_after_write, "the first write() must fire the deferred start()");
}

/* Write-then-start (the legal order every in-tree caller used to need)
 * must keep working unchanged: the block is already queued, so start()
 * triggers immediately via z_start()'s IMMEDIATE-trigger branch (as
 * opposed to the deferred-then-retried path every other test in this
 * file exercises). A follow-up write() afterward pins MAJOR-2's
 * invariant fix on THIS specific branch: _mark_tx_started() must have
 * cleared tx_pending_start (it was never set in this sequence, so this
 * is a no-op clear, but it proves the call site is wired the same way
 * as the retry path) -- if it hadn't, the follow-up write would
 * spuriously retry a START on an already-running stream and get
 * i2s_dw's -EIO instead of just writing. */
ZTEST(alp_i2s_start_defer, test_write_then_start_succeeds)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();

	alp_status_t write_rc             = write_block(h);
	bool         running_before_start = fake_i2s_tx_running();
	alp_status_t start_rc             = alp_i2s_start(h);
	bool         running_after_start  = fake_i2s_tx_running();
	alp_status_t write2_rc            = write_block(h);
	bool         running_after_write2 = fake_i2s_tx_running();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(write_rc, ALP_OK);
	zassert_false(running_before_start, "a write with no start() pending must not self-trigger");
	zassert_equal(start_rc, ALP_OK);
	zassert_true(running_after_start, "start() with a block already queued must trigger now");
	zassert_equal(write2_rc,
	              ALP_OK,
	              "MAJOR-2: a write after the immediate-trigger start() must not spuriously "
	              "retry a START on an already-running stream");
	zassert_true(running_after_write2);
}

/* Stopping a pending-but-never-written stream must not issue a real
 * trigger that fails -- i2s_dw.c's STOP/DRAIN refuse a stream that
 * never reached RUNNING. */
ZTEST(alp_i2s_start_defer, test_start_then_stop_with_no_write_is_ok)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();

	alp_status_t start_rc = alp_i2s_start(h);
	alp_status_t stop_rc  = alp_i2s_stop(h);
	bool         running  = fake_i2s_tx_running();
	size_t       depth    = fake_i2s_tx_queue_depth();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(start_rc, ALP_OK);
	zassert_equal(stop_rc, ALP_OK, "stopping a never-started (pending) stream must not -EIO");
	zassert_false(running);
	zassert_equal(depth, 0u);
}

/* Finding 3's exact repro: write, write (slab now full, 2/2), stop
 * WITHOUT ever calling start() -- must return ALP_OK and must not
 * strand the two queued blocks (the old bug: DRAIN -EIO'd, or a
 * flags-only "clear and pretend" stop left them stuck in the ring
 * holding slab memory forever). The next start()+write() must still
 * work -- proves the blocks were actually released, not just forgotten
 * about. */
ZTEST(alp_i2s_start_defer, test_write_write_stop_start_write_stranded_blocks)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();

	alp_status_t write1_rc         = write_block(h);
	alp_status_t write2_rc         = write_block(h); /* fills the 2-block slab */
	size_t       depth_before_stop = fake_i2s_tx_queue_depth();
	alp_status_t stop_rc           = alp_i2s_stop(h);
	size_t       depth_after_stop  = fake_i2s_tx_queue_depth();
	alp_status_t start_rc          = alp_i2s_start(h);
	alp_status_t write3_rc         = write_block(h);
	bool         running           = fake_i2s_tx_running();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(write1_rc, ALP_OK);
	zassert_equal(write2_rc, ALP_OK);
	zassert_equal(depth_before_stop, 2u, "both writes must have genuinely queued");
	zassert_equal(stop_rc, ALP_OK, "stop() with no start() ever called must still succeed");
	zassert_equal(depth_after_stop, 0u, "stop() must release the stranded blocks, not just OK");
	zassert_equal(start_rc, ALP_OK);
	zassert_equal(write3_rc,
	              ALP_OK,
	              "a write after the stop/start must succeed -- would time out on k_mem_slab_"
	              "alloc if the earlier 2 blocks were never freed");
	zassert_true(running);
}

/* start -> write (fires) -> stop (DRAIN on a genuinely running stream)
 * -> start -> write: the normal running-stream stop path must still
 * work exactly as before, and the deferred-start dance must be able to
 * run a second full cycle. */
ZTEST(alp_i2s_start_defer, test_start_write_stop_running_start_write)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();

	alp_status_t start1_rc            = alp_i2s_start(h);
	alp_status_t write1_rc            = write_block(h);
	bool         running_after_write1 = fake_i2s_tx_running();
	alp_status_t stop_rc              = alp_i2s_stop(h);
	bool         running_after_stop   = fake_i2s_tx_running();
	alp_status_t start2_rc            = alp_i2s_start(h);
	bool         running_after_start2 = fake_i2s_tx_running();
	alp_status_t write2_rc            = write_block(h);
	bool         running_after_write2 = fake_i2s_tx_running();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(start1_rc, ALP_OK);
	zassert_equal(write1_rc, ALP_OK);
	zassert_true(running_after_write1, "first cycle must have reached RUNNING");
	zassert_equal(stop_rc, ALP_OK, "DRAIN on a genuinely running stream must still succeed");
	zassert_false(running_after_stop);
	zassert_equal(start2_rc, ALP_OK, "second start() with nothing queued must defer again");
	zassert_false(running_after_start2, "must not have self-triggered");
	zassert_equal(write2_rc, ALP_OK);
	zassert_true(running_after_write2, "second write() must fire the second deferred start()");
}

/* Finding 4 (UAF): write -> close (no start ever called) -> reopen ->
 * start -> write must not dequeue a stale pointer into memory the
 * first handle's close() already freed. Deliberately no fake_i2s_reset()
 * between the two opens -- the fake's ring is a device-global resource
 * that persists across alp_i2s_open()/close() exactly like the real
 * i2s_dw device's ring buffer does, and that persistence is the point:
 * proving the SECOND handle's start() sees an EMPTY ring (because
 * close() correctly drained the first handle's stale block) is the
 * direct, non-corrupting way to prove no stale block/UAF -- a real
 * dangling-pointer free is unsafe to provoke deliberately in a test. */
ZTEST(alp_i2s_start_defer, test_write_close_reopen_start_write_no_stale_block)
{
	fake_i2s_reset();

	alp_i2s_t   *h1        = open_tx();
	alp_status_t write1_rc = write_block(h1); /* queued, start() never called */
	alp_i2s_close(h1);
	size_t depth_after_h1_close = fake_i2s_tx_queue_depth();

	alp_i2s_t   *h2        = open_tx();
	alp_status_t start2_rc = alp_i2s_start(h2);
	alp_status_t write2_rc = write_block(h2);
	bool         running   = fake_i2s_tx_running();
	size_t       depth_end = fake_i2s_tx_queue_depth();

	alp_i2s_close(h2);

	zassert_not_null(h1, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(write1_rc, ALP_OK);
	/* Pre-fix (close() gated DROP on h->started, which was false here):
	 * this would still be 1, the stale block from h1's now-freed slab. */
	zassert_equal(depth_after_h1_close,
	              0u,
	              "close() must release a never-started handle's queued block, not strand it "
	              "for the next open() to dequeue into freed memory");
	zassert_not_null(h2, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(start2_rc, ALP_OK);
	zassert_equal(write2_rc, ALP_OK);
	zassert_true(running, "h2's own write() must have fired its own deferred start");
	zassert_equal(depth_end, 0u, "h2's block was consumed, not stuck either");
}

/* Finding 2 / MAJOR-1 (issue #2132 review): a START failure discovered
 * only once data is queued must surface from the write() call that
 * queued it (never ALP_OK), and EVERY later write while still pending
 * must retry -- not just the first one or two. This runs 5 (> the
 * 2-block slab depth) consecutive forced failures: since MAJOR-1's fix
 * DROPs the block a failed retry just queued before returning, the
 * slab must NEVER fill up under sustained failures -- every one of the
 * 5 writes must fail with the FORCED status (ALP_ERR_IO), never
 * ALP_ERR_TIMEOUT from k_mem_slab_alloc() starving on un-started
 * blocks, and the ring must be empty after each one. */
ZTEST(alp_i2s_start_defer, test_start_failure_retries_on_every_write_past_slab_depth)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();

	alp_status_t start_rc = alp_i2s_start(h);

	enum { N_FAILURES = 5 }; /* > CONFIG_ALP_SDK_MAX_I2S_HANDLES's slab depth of 2 */
	fake_i2s_force_start_fail(-EIO, N_FAILURES);
	alp_status_t write_rc[N_FAILURES];
	size_t       depth_after[N_FAILURES];
	bool         running_after[N_FAILURES];
	for (int i = 0; i < N_FAILURES; ++i) {
		write_rc[i]      = write_block(h);
		depth_after[i]   = fake_i2s_tx_queue_depth();
		running_after[i] = fake_i2s_tx_running();
	}

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(start_rc, ALP_OK, "deferred start must not fail up front");
	for (int i = 0; i < N_FAILURES; ++i) {
		zassert_equal(write_rc[i],
		              ALP_ERR_IO,
		              "write %d into a stream whose START keeps failing must surface the "
		              "FORCED status, not ALP_OK and not a slab-exhaustion ALP_ERR_TIMEOUT",
		              i);
		zassert_equal(depth_after[i], 0u, "write %d must not strand a block in the ring", i);
		zassert_false(running_after[i]);
	}
}

/* Companion to the above: once the injected fault clears, a fresh
 * start()+write() cycle must recover normally (the earlier failures
 * must not have wedged tx_pending_start permanently true or false). */
ZTEST(alp_i2s_start_defer, test_start_failure_recovers_after_fault_clears)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();

	alp_status_t start1_rc = alp_i2s_start(h);
	fake_i2s_force_start_fail(-EIO, 1u);
	alp_status_t write1_rc       = write_block(h);
	bool         running_after_1 = fake_i2s_tx_running();
	/* MAJOR-1: the failed retry already DROPped its own block -- nothing
	 * piles up to "release" here any more. */
	size_t depth_after_1 = fake_i2s_tx_queue_depth();

	/* Fault is one-shot (count exhausted) -- a stop() here is not load-
	 * bearing for cleanup any more (see above), just a normal idle-reset
	 * before demonstrating a clean recovery cycle. */
	alp_status_t stop_rc     = alp_i2s_stop(h);
	alp_status_t start2_rc   = alp_i2s_start(h);
	alp_status_t write2_rc   = write_block(h);
	bool         running_end = fake_i2s_tx_running();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(start1_rc, ALP_OK);
	zassert_not_equal(write1_rc, ALP_OK, "forced first attempt must fail");
	zassert_false(running_after_1);
	zassert_equal(depth_after_1, 0u, "MAJOR-1: the failed retry must not strand a block");
	zassert_equal(stop_rc, ALP_OK);
	zassert_equal(start2_rc, ALP_OK);
	zassert_equal(write2_rc, ALP_OK, "a clean write after the fault clears must succeed");
	zassert_true(running_end);
}

/* MAJOR-2 (issue #2132 review): the original probe sequence -- start
 * (deferred) -> write (forced START-retry failure) -> start (explicit
 * "retry") -> write -- used to leave tx_pending_start stuck true past a
 * successful start() on the immediate-trigger path (z_start() set
 * tx_started without clearing tx_pending_start), so the SECOND write
 * would retry a START on an already-RUNNING stream and get i2s_dw's
 * -EIO instead of just writing.
 *
 * MAJOR-1's fix (DROP the block on a failed retry) closes off this
 * EXACT sequence as a live repro: after write1's DROP, tx_block_queued
 * is false again, so start2 takes the DEFER branch (not the immediate-
 * trigger one) and does not touch hardware -- there is no longer a
 * window where the immediate-trigger path can observe tx_pending_start
 * still true, because z_write() always resolves tx_pending_start (to
 * either "started" or "still pending, block dropped") within the SAME
 * call that set tx_block_queued, before any other call can observe it.
 * See _mark_tx_started()'s comment for the invariant this establishes
 * BY CONSTRUCTION regardless -- kept as defense-in-depth (protects any
 * future caller of z_start()'s immediate branch, e.g. a refactor that
 * reintroduces a queued-but-still-pending state).
 *
 * This test still pins the REQUIRED END-TO-END OUTCOME of the original
 * sequence (the stream must end up genuinely playing, not permanently
 * refusing writes), documenting that the INTERNAL mechanism changed:
 * start2 now defers again (pending stays true) and write2 is what
 * actually fires the real trigger, rather than start2 firing it
 * directly. */
ZTEST(alp_i2s_start_defer, test_explicit_restart_after_failed_retry_ends_up_running)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();

	alp_status_t start1_rc = alp_i2s_start(h);
	fake_i2s_force_start_fail(-EIO, 1u);
	alp_status_t write1_rc            = write_block(h);
	alp_status_t start2_rc            = alp_i2s_start(h);
	bool         running_after_start2 = fake_i2s_tx_running();
	alp_status_t write2_rc            = write_block(h);
	bool         running_after_write2 = fake_i2s_tx_running();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(start1_rc, ALP_OK);
	zassert_not_equal(write1_rc, ALP_OK, "forced first attempt must fail");
	zassert_equal(start2_rc, ALP_OK, "the explicit restart must not fail");
	/* Post-MAJOR-1: start2 defers again (nothing queued yet), so it must
	 * NOT be running until write2 fires it. */
	zassert_false(running_after_start2);
	zassert_equal(write2_rc, ALP_OK, "write into what must become a running stream must not fail");
	zassert_true(running_after_write2);
}

/* issue #2132 review round 4: the start/write-vs-stop race the round-3
 * fix's own lock comment flagged as still open. Single-threaded repro via
 * fake_i2s_set_write_hook() (see its own comment) -- the hook runs
 * synchronously from INSIDE the real i2s_write() call z_write() makes,
 * before z_write() ever reaches its own lock acquisition, so calling
 * alp_i2s_stop() from the hook deterministically reproduces "a concurrent
 * stop() wins the race and DROPs this write's block before this write()
 * records tx_block_queued=true":
 *
 *   1. start() defers (ring empty).
 *   2. write() queues its block for real, then (via the hook, standing in
 *      for a second thread) stop() runs to completion FIRST: it sees
 *      tx_started still false, DROPs the block THIS write just queued,
 *      and clears all three flags.
 *   3. write()'s OWN code, only now reaching its lock acquisition, stamps
 *      tx_block_queued = true over what is -- for real -- an empty ring,
 *      and (since tx_pending_start is now false, cleared by the hook's
 *      stop()) returns ALP_OK without ever noticing.
 *   4. The next start() sees the STALE tx_block_queued = true and takes
 *      the immediate-trigger path straight into i2s_dw's real empty-ring
 *      -ENOMEM -- the original #2132 symptom, reappearing via a race.
 *
 * z_start()'s NOMEM-as-stale-flag handling is what must catch step 4 and
 * recover instead of propagating it. */
static alp_i2s_t *race_handle;

static void race_stop_hook(void)
{
	(void)alp_i2s_stop(race_handle);
}

ZTEST(alp_i2s_start_defer, test_start_write_stop_race_stale_flag_recovers)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();
	race_handle  = h;

	alp_status_t start1_rc = alp_i2s_start(h);
	fake_i2s_set_write_hook(race_stop_hook);
	alp_status_t write1_rc            = write_block(h); /* races the hook mid-call */
	bool         running_after_write1 = fake_i2s_tx_running();

	/* Pre-fix: this returned ALP_ERR_NOMEM here -- the -7 symptom via a
	 * race, on a codebase where the single-threaded tests all pass. */
	alp_status_t start2_rc            = alp_i2s_start(h);
	bool         running_after_start2 = fake_i2s_tx_running();
	alp_status_t write2_rc            = write_block(h);
	bool         running_after_write2 = fake_i2s_tx_running();

	alp_i2s_close(h);
	fake_i2s_set_write_hook(NULL);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(start1_rc, ALP_OK);
	zassert_equal(write1_rc, ALP_OK, "the write itself queued fine before the race hit");
	zassert_false(running_after_write1, "the raced-in stop() must have DROPped it for real");
	zassert_equal(start2_rc,
	              ALP_OK,
	              "a stale tx_block_queued must not surface i2s_dw's empty-ring -ENOMEM to "
	              "the caller");
	zassert_false(running_after_start2, "recovery re-defers -- nothing is queued yet");
	zassert_equal(write2_rc, ALP_OK);
	zassert_true(running_after_write2, "a normal write after the recovery must reach RUNNING");
}

/* issue #2137, case (a): start, write, underrun, then stop must recover
 * (return ALP_OK) instead of the -EIO i2s_dw_trigger()'s DRAIN case
 * returns forever once the stream is stuck in I2S_STATE_ERROR --
 * silicon repro: alp_audio_out_stop() returning -5 after the queue ran
 * dry, E1M-AEN803 serial 2026W36-0002. */
ZTEST(alp_i2s_start_defer, test_underrun_then_stop_recovers)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();

	alp_status_t start_rc            = alp_i2s_start(h);
	alp_status_t write_rc            = write_block(h);
	bool         running_after_write = fake_i2s_tx_running();

	fake_i2s_tx_simulate_underrun();
	bool in_error_after_underrun = fake_i2s_tx_in_error();

	alp_status_t stop_rc             = alp_i2s_stop(h);
	bool         running_after_stop  = fake_i2s_tx_running();
	bool         in_error_after_stop = fake_i2s_tx_in_error();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(start_rc, ALP_OK);
	zassert_equal(write_rc, ALP_OK);
	zassert_true(running_after_write);
	zassert_true(in_error_after_underrun, "the underrun must have reached I2S_STATE_ERROR");
	zassert_equal(stop_rc,
	              ALP_OK,
	              "stop() after an underrun must recover via the DROP fallback once DRAIN "
	              "-EIOs, not propagate i2s_dw's -EIO forever");
	zassert_false(running_after_stop);
	zassert_false(in_error_after_stop, "stop()'s DROP fallback must clear ERROR");
}

/* issue #2137, case (b): underrun, then start() alone (deferred again --
 * PREPARE drops the ring, so nothing is queued right after) plus a
 * follow-up write() (which fires the real trigger) must play again --
 * silicon repro: alp_audio_out_start() returning -5 after the underrun,
 * E1M-AEN803 serial 2026W36-0002. */
ZTEST(alp_i2s_start_defer, test_underrun_then_start_and_write_replays)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();

	alp_status_t start1_rc            = alp_i2s_start(h);
	alp_status_t write1_rc            = write_block(h);
	bool         running_after_write1 = fake_i2s_tx_running();

	fake_i2s_tx_simulate_underrun();
	bool in_error_after_underrun = fake_i2s_tx_in_error();

	/* start() alone, with no write() in between, must NOT fail --
	 * i2s_dw_trigger()'s START case -EIOs from ERROR; PREPARE recovers
	 * to READY, and since PREPARE also drops the (already empty) ring,
	 * the retried START correctly re-defers rather than hitting
	 * -ENOMEM. */
	alp_status_t start2_rc            = alp_i2s_start(h);
	bool         running_after_start2 = fake_i2s_tx_running();
	bool         error_after_start2   = fake_i2s_tx_in_error();

	alp_status_t write2_rc            = write_block(h);
	bool         running_after_write2 = fake_i2s_tx_running();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(start1_rc, ALP_OK);
	zassert_equal(write1_rc, ALP_OK);
	zassert_true(running_after_write1);
	zassert_true(in_error_after_underrun, "the underrun must have reached I2S_STATE_ERROR");
	zassert_equal(start2_rc, ALP_OK, "start() after an underrun must recover, not -EIO");
	zassert_false(running_after_start2, "nothing is queued yet -- must re-defer, not fake it");
	zassert_false(error_after_start2, "PREPARE must have cleared I2S_STATE_ERROR");
	zassert_equal(write2_rc, ALP_OK);
	zassert_true(running_after_write2, "write() must have fired the re-armed deferred start()");
}

/* issue #2137, case (c): underrun, then write() alone (no explicit
 * start()) must resume playback -- the exact "gap between writes
 * breaks the stream" symptom the issue reports -- and must neither
 * leak the retried block nor double-free it, proven against the REAL
 * k_mem_slab's own free-block count, not just this fake's bookkeeping. */
ZTEST(alp_i2s_start_defer, test_underrun_then_write_resumes_with_no_slab_leak)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();

	alp_status_t start_rc             = alp_i2s_start(h);
	alp_status_t write1_rc            = write_block(h);
	bool         running_after_write1 = fake_i2s_tx_running();
	size_t       free_before_underrun = fake_i2s_slab_free_count();

	fake_i2s_tx_simulate_underrun();
	bool in_error_after_underrun = fake_i2s_tx_in_error();

	alp_status_t write2_rc            = write_block(h);
	bool         running_after_write2 = fake_i2s_tx_running();
	size_t       free_after_write2    = fake_i2s_slab_free_count();
	size_t       depth_after_write2   = fake_i2s_tx_queue_depth();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(start_rc, ALP_OK);
	zassert_equal(write1_rc, ALP_OK);
	zassert_true(running_after_write1);
	zassert_true(in_error_after_underrun, "the underrun must have reached I2S_STATE_ERROR");
	zassert_equal(write2_rc,
	              ALP_OK,
	              "a write after an underrun must resume playback, not propagate -EIO forever");
	zassert_true(running_after_write2, "the retried write must have re-fired START");
	zassert_equal(depth_after_write2, 0u, "the re-armed START must have consumed the block");
	zassert_equal(free_after_write2,
	              free_before_underrun,
	              "the slab's free-block count must return to where it was -- the retried "
	              "block was reused, not leaked, and never double-freed");
}

/* issue #2137, case (d): PREPARE itself refused must surface the
 * ORIGINAL write failure (mapped from the underrun's -EIO), not
 * PREPARE's own (a distinct forced errno, on purpose, so a bug that
 * swapped the two would be caught), and must free the block it can no
 * longer deliver -- nothing downstream will ever consume it now.
 *
 * On a backend with NO recovery logic at all (pre-#2137), write2_rc and
 * the slab-free-count check below would ALSO happen to come out this
 * way -- passing straight through never queues anything and never
 * touches PREPARE either. fake_i2s_prepare_call_count() is the
 * discriminator that actually proves this backend attempted recovery
 * and PREPARE was the thing that failed, not that recovery was simply
 * never attempted. */
ZTEST(alp_i2s_start_defer, test_underrun_write_prepare_refused_propagates_original_error)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();

	alp_status_t start_rc             = alp_i2s_start(h);
	alp_status_t write1_rc            = write_block(h);
	bool         running_after_write1 = fake_i2s_tx_running();
	size_t       free_before          = fake_i2s_slab_free_count();

	fake_i2s_tx_simulate_underrun();
	fake_i2s_force_prepare_fail(-ETIMEDOUT, 1u);

	alp_status_t write2_rc     = write_block(h);
	size_t       free_after    = fake_i2s_slab_free_count();
	size_t       prepare_calls = fake_i2s_prepare_call_count();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(start_rc, ALP_OK);
	zassert_equal(write1_rc, ALP_OK);
	zassert_true(running_after_write1);
	zassert_equal(prepare_calls,
	              1u,
	              "the write must have actually attempted PREPARE, not skipped recovery "
	              "entirely and happened to land on the same status by coincidence");
	zassert_equal(write2_rc,
	              ALP_ERR_IO,
	              "PREPARE failing must surface the ORIGINAL write's -EIO (ALP_ERR_IO), not "
	              "PREPARE's own forced -ETIMEDOUT");
	zassert_equal(free_after, free_before, "the undeliverable block must be freed, not leaked");
}

/* RX must be unaffected by any of the above: rx_stream_start() allocates
 * its own block from the slab instead of dequeuing a caller-filled
 * ring, so z_start() never defers for RX -- direct alp_i2s_start()
 * before any read() must trigger immediately. */
ZTEST(alp_i2s_start_defer, test_rx_start_is_never_deferred)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_rx();

	/* A fresh, never-triggered RX handle's trigger() always succeeds --
	 * what this test actually pins is that z_start() took the immediate
	 * path, not the TX-only deferral branch, for an RX handle. */
	alp_status_t start_rc = alp_i2s_start(h);
	alp_status_t stop_rc  = alp_i2s_stop(h);

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (RX)");
	zassert_equal(start_rc, ALP_OK);
	zassert_equal(stop_rc, ALP_OK);
}

/* issue #2137: stop() on an RX handle that was NEVER started must
 * return ALP_OK directly, without ever issuing a real DROP trigger --
 * DROP tears down real RX hardware (rx_stream_disable(), including its
 * own clock teardown), so it must not run for a handle the caller never
 * armed. Asserting fake_i2s_drop_call_count() == 0 (not just stop_rc ==
 * ALP_OK) is what actually pins the gate: a mutant that changes the
 * gated `return ALP_OK;` to `return rc;` (DRAIN's own -EIO) would fail
 * on stop_rc alone, but a mutant that simply REMOVES the gate and lets
 * DROP run unconditionally would still return ALP_OK (DROP always
 * succeeds in this fake) and pass a stop_rc-only check. */
ZTEST(alp_i2s_start_defer, test_rx_stop_never_started_is_ok_and_skips_drop)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_rx();

	alp_status_t stop_rc    = alp_i2s_stop(h);
	size_t       drop_calls = fake_i2s_drop_call_count();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (RX)");
	zassert_equal(stop_rc, ALP_OK, "a never-started RX stop must return ALP_OK directly");
	zassert_equal(
	    drop_calls, 0u, "a never-started RX handle must never reach the real DROP trigger");
}

/* issue #2137 review round 2, finding 2: the NOMEM branch must clear
 * tx_started too, not just tx_block_queued/tx_pending_start -- exact
 * repro: start() (deferred) -> write() fires the real START -> underrun
 * -> start(): the real START -EIOs, PREPARE succeeds, the retried START
 * -ENOMEMs (PREPARE dropped the already-empty ring). Proven indirectly
 * via fake_i2s_drain_call_count(): a stale tx_started=true would make
 * z_stop() (finding 2/3's own new reader) issue a REAL DRAIN that
 * i2s_dw would refuse (the stream is genuinely READY, not RUNNING) --
 * the fixed backend must see tx_started already false and skip that
 * doomed call entirely. */
ZTEST(alp_i2s_start_defer, test_underrun_start_retry_clears_started_invariant)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();

	alp_status_t start1_rc = alp_i2s_start(h);
	alp_status_t write1_rc = write_block(h);
	fake_i2s_tx_simulate_underrun();

	alp_status_t start2_rc = alp_i2s_start(h);

	size_t       drain_calls_before_stop = fake_i2s_drain_call_count();
	alp_status_t stop_rc                 = alp_i2s_stop(h);
	size_t       drain_calls_after_stop  = fake_i2s_drain_call_count();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(start1_rc, ALP_OK);
	zassert_equal(write1_rc, ALP_OK);
	zassert_equal(start2_rc, ALP_OK, "recovery-then-empty-ring-defer must not fail");
	zassert_equal(drain_calls_before_stop, 0u, "nothing before this point issues a real DRAIN");
	zassert_equal(drain_calls_after_stop,
	              drain_calls_before_stop,
	              "the NOMEM branch must clear tx_started -- a stale true would make z_stop() "
	              "issue a real DRAIN this fake (and i2s_dw) would just refuse");
	zassert_equal(stop_rc, ALP_OK);
}

/* issue #2137 review round 2, finding 1: the RX-side leak this issue
 * shipped alongside -- zephyr/drivers/i2s/i2s_dw.c's RX IRQ handler
 * orphaning the just-filled block on EITHER of its two error exits
 * (a failed k_mem_slab_alloc() for the next block, or a failed
 * queue_put() of this one) instead of freeing it. With the 2-block slab
 * this backend allocates, TWO leaked overruns exhaust it and the THIRD
 * start()'s retried START -ENOMEMs permanently (RX has no NOMEM-stale-
 * flag branch -- that is TX-only, see z_start()). Proven against the
 * REAL k_mem_slab's own free-block count, not just the fake's
 * bookkeeping, across three full overrun-and-recover cycles, then a
 * genuine completed-frame read() to prove the recovery is not just
 * slab-count bookkeeping but an actually-working RX path. */
ZTEST(alp_i2s_start_defer, test_rx_overrun_recovers_no_slab_leak)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_rx();

	size_t free_at_open = fake_i2s_rx_slab_free_count();

	alp_status_t start1_rc = alp_i2s_start(h);
	fake_i2s_rx_simulate_overrun();
	bool   in_error_1        = fake_i2s_rx_in_error();
	size_t free_after_cycle1 = fake_i2s_rx_slab_free_count();

	alp_status_t start2_rc = alp_i2s_start(h);
	fake_i2s_rx_simulate_overrun();
	bool   in_error_2        = fake_i2s_rx_in_error();
	size_t free_after_cycle2 = fake_i2s_rx_slab_free_count();

	alp_status_t start3_rc = alp_i2s_start(h);
	fake_i2s_rx_simulate_overrun();
	size_t free_after_cycle3 = fake_i2s_rx_slab_free_count();

	/* Final recovery cycle: stays running this time and actually
	 * completes a frame, proving read() still works after 3 leak-prone
	 * cycles. */
	alp_status_t start4_rc = alp_i2s_start(h);
	fake_i2s_rx_complete_block();
	static uint8_t buf[BLOCK_BYTES];
	size_t         got     = 0u;
	alp_status_t   read_rc = alp_i2s_read(h, buf, sizeof(buf), &got, 100u);

	alp_status_t stop_rc         = alp_i2s_stop(h);
	size_t       free_after_stop = fake_i2s_rx_slab_free_count();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (RX)");
	zassert_equal(start1_rc, ALP_OK);
	zassert_true(in_error_1, "the overrun must have reached I2S_STATE_ERROR");
	zassert_equal(free_after_cycle1, free_at_open, "cycle 1 must not leak");
	zassert_equal(start2_rc, ALP_OK, "an RX overrun must recover via PREPARE, not -EIO forever");
	zassert_true(in_error_2);
	zassert_equal(free_after_cycle2, free_at_open, "cycle 2 must not leak");
	zassert_equal(start3_rc,
	              ALP_OK,
	              "a leaked block from either earlier cycle would starve the 2-block slab here");
	zassert_equal(free_after_cycle3, free_at_open, "cycle 3 must not leak either");
	zassert_equal(start4_rc, ALP_OK);
	zassert_equal(read_rc, ALP_OK, "a genuine frame must still be readable after recovery");
	zassert_equal(got, (size_t)BLOCK_BYTES);
	zassert_equal(stop_rc, ALP_OK);
	zassert_equal(free_after_stop, free_at_open, "stop() must release the final active block too");
}

/* issue #2137 review round 2, finding 5: start() with PREPARE refused
 * must surface the ORIGINAL START failure (ALP_ERR_IO), not PREPARE's
 * own forced status -- the start()-side sibling of case (d)'s write()
 * coverage. prepare_calls proves the recovery was actually attempted. */
ZTEST(alp_i2s_start_defer, test_underrun_start_prepare_refused_propagates_original_error)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();

	alp_status_t start1_rc = alp_i2s_start(h);
	alp_status_t write1_rc = write_block(h);
	fake_i2s_tx_simulate_underrun();
	fake_i2s_force_prepare_fail(-ETIMEDOUT, 1u);

	alp_status_t start2_rc     = alp_i2s_start(h);
	size_t       prepare_calls = fake_i2s_prepare_call_count();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(start1_rc, ALP_OK);
	zassert_equal(write1_rc, ALP_OK);
	zassert_equal(prepare_calls,
	              1u,
	              "start() must have actually attempted PREPARE, not skipped recovery and "
	              "happened to land on the same status by coincidence");
	zassert_equal(start2_rc,
	              ALP_ERR_IO,
	              "a refused PREPARE must surface the ORIGINAL START's -EIO (ALP_ERR_IO), not "
	              "PREPARE's own forced -ETIMEDOUT");
}

/* issue #2137 review round 2, finding 5: the RETRIED write() (the one
 * that runs after a SUCCESSFUL PREPARE) can itself fail for an unrelated
 * reason -- must surface THAT failure (not the original underrun's
 * -EIO) and must still free the block exactly once (no leak, no double
 * free), proven against the real k_mem_slab. */
ZTEST(alp_i2s_start_defer, test_underrun_write_retry_fails_after_prepare_succeeds_no_leak)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();

	alp_status_t start_rc    = alp_i2s_start(h);
	alp_status_t write1_rc   = write_block(h);
	size_t       free_before = fake_i2s_slab_free_count();

	fake_i2s_tx_simulate_underrun();
	fake_i2s_force_write_fail(-ETIMEDOUT, 1u);

	alp_status_t write2_rc     = write_block(h);
	size_t       free_after    = fake_i2s_slab_free_count();
	size_t       prepare_calls = fake_i2s_prepare_call_count();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(start_rc, ALP_OK);
	zassert_equal(write1_rc, ALP_OK);
	zassert_equal(prepare_calls,
	              1u,
	              "PREPARE must have been attempted (and succeeded -- this test forces the "
	              "RETRY write to fail, not PREPARE itself)");
	zassert_equal(write2_rc,
	              ALP_ERR_TIMEOUT,
	              "a forced failure on the RETRY (post-PREPARE) write must surface AS ITSELF, "
	              "not the original underrun's -EIO");
	zassert_equal(free_after,
	              free_before,
	              "the undeliverable block must be freed exactly once -- not leaked, not "
	              "double-freed");
}

/* issue #2137 review round 3, finding 1/2: real API interleavings against
 * the actual race window z_write() has (between this write()'s own
 * i2s_write() failing -EIO and this write()'s own PREPARE running, both
 * OUTSIDE the backend lock at that point) -- run via
 * fake_i2s_set_write_fail_hook(), which fires a REAL alp_i2s_* call on the
 * SAME handle from exactly that window, standing in for a second thread
 * without needing real ones. These reproduce the round-2 blocker directly:
 * an unconditional flag reset on a REFUSED prepare stamps a fresh "needs
 * restart" over flags a CONCURRENT call had already set correctly (every
 * state-changing trigger updates the tx_* flags under the same lock it
 * uses, so by the time this call's own PREPARE runs, the flags already
 * reflect whatever that concurrent call actually did). */
static alp_i2s_t   *ilv_handle;
static alp_status_t ilv_inner_rc;
static bool         ilv_running_after_inner;

static void ilv_writer_hook(void)
{
	ilv_inner_rc            = write_block(ilv_handle);
	ilv_running_after_inner = fake_i2s_tx_running();
}

static void ilv_stop_hook(void)
{
	ilv_inner_rc            = alp_i2s_stop(ilv_handle);
	ilv_running_after_inner = fake_i2s_tx_running();
}

static void ilv_start_hook(void)
{
	ilv_inner_rc            = alp_i2s_start(ilv_handle);
	ilv_running_after_inner = fake_i2s_tx_running();
}

/* Writer A's own write() races ahead and fully recovers (PREPARE succeeds,
 * retry queues, START fires -- A ends up genuinely RUNNING) from INSIDE
 * writer B's failed i2s_write(), before B's own PREPARE runs. B's retry
 * must land on A's now-RUNNING stream and be accepted normally -- NOT tear
 * it down by stamping a stale "needs restart" over A's valid tx_started. */
ZTEST(alp_i2s_start_defer, test_ilv_a_second_writer_after_concurrent_writer_recovery)
{
	fake_i2s_reset();
	alp_i2s_t *h            = open_tx();
	ilv_handle              = h;
	ilv_inner_rc            = -999;
	ilv_running_after_inner = false;
	alp_status_t start_rc   = alp_i2s_start(h);
	alp_status_t w1_rc      = write_block(h);
	fake_i2s_tx_simulate_underrun();
	bool in_err = fake_i2s_tx_in_error();
	fake_i2s_set_write_fail_hook(ilv_writer_hook);
	alp_status_t wb_rc           = write_block(h);
	bool         running_after_b = fake_i2s_tx_running();
	size_t       depth           = fake_i2s_tx_queue_depth();
	size_t       free_after      = fake_i2s_slab_free_count();
	alp_i2s_close(h);
	fake_i2s_set_write_fail_hook(NULL);
	TC_PRINT("ILV-A start=%d w1=%d in_err=%d writerA=%d running_after_A=%d writerB=%d "
	         "running_after_B=%d depth=%zu free=%zu\n",
	         (int)start_rc,
	         (int)w1_rc,
	         (int)in_err,
	         (int)ilv_inner_rc,
	         (int)ilv_running_after_inner,
	         (int)wb_rc,
	         (int)running_after_b,
	         depth,
	         free_after);
	zassert_equal(ilv_inner_rc, ALP_OK);
	zassert_true(ilv_running_after_inner);
	zassert_true(running_after_b, "writer B must not tear down the RUNNING stream of writer A");
	zassert_equal(wb_rc, ALP_OK, "writer B retry landed on a RUNNING stream, must be accepted");
}

/* A concurrent stop() wins the race and genuinely stops the stream (DROP
 * succeeds) from inside this writer's failed i2s_write(), before this
 * writer's own PREPARE runs. This writer's retry must NOT resurrect
 * playback the caller just explicitly stopped -- only the caller's own
 * next start() should. */
ZTEST(alp_i2s_start_defer, test_ilv_b_writer_does_not_undo_concurrent_stop)
{
	fake_i2s_reset();
	alp_i2s_t *h            = open_tx();
	ilv_handle              = h;
	ilv_inner_rc            = -999;
	ilv_running_after_inner = true;
	alp_status_t start_rc   = alp_i2s_start(h);
	alp_status_t w1_rc      = write_block(h);
	fake_i2s_tx_simulate_underrun();
	fake_i2s_set_write_fail_hook(ilv_stop_hook);
	alp_status_t wb_rc                = write_block(h);
	bool         running_after_b      = fake_i2s_tx_running();
	alp_status_t start2_rc            = alp_i2s_start(h);
	bool         running_after_start2 = fake_i2s_tx_running();
	alp_i2s_close(h);
	fake_i2s_set_write_fail_hook(NULL);
	TC_PRINT("ILV-B start=%d w1=%d stop=%d running_after_stop=%d writer=%d "
	         "running_after_writer=%d start2=%d running_after_start2=%d\n",
	         (int)start_rc,
	         (int)w1_rc,
	         (int)ilv_inner_rc,
	         (int)ilv_running_after_inner,
	         (int)wb_rc,
	         (int)running_after_b,
	         (int)start2_rc,
	         (int)running_after_start2);
	zassert_equal(ilv_inner_rc, ALP_OK);
	zassert_false(ilv_running_after_inner);
	zassert_false(running_after_b, "a writer that raced stop() must not restart playback");
	zassert_equal(start2_rc, ALP_OK, "the start() the caller issues after stop() must not fail");
}

/* A concurrent start() wins the race and fully recovers the stream (its
 * own PREPARE succeeds, ring is empty so it re-defers) from inside this
 * writer's failed i2s_write(), before this writer's own PREPARE runs. This
 * writer's retry must still succeed and resume playback -- the concurrent
 * start() alone left nothing queued, so this write is what actually fires
 * the real trigger. */
ZTEST(alp_i2s_start_defer, test_ilv_d_writer_resumes_after_concurrent_start_recovery)
{
	fake_i2s_reset();
	alp_i2s_t *h            = open_tx();
	ilv_handle              = h;
	ilv_inner_rc            = -999;
	ilv_running_after_inner = true;
	alp_status_t start_rc   = alp_i2s_start(h);
	alp_status_t w1_rc      = write_block(h);
	fake_i2s_tx_simulate_underrun();
	fake_i2s_set_write_fail_hook(ilv_start_hook);
	alp_status_t wb_rc           = write_block(h);
	bool         running_after_b = fake_i2s_tx_running();
	alp_i2s_close(h);
	fake_i2s_set_write_fail_hook(NULL);
	TC_PRINT("ILV-D start=%d w1=%d start2=%d running_after_start2=%d writer=%d "
	         "running_after_writer=%d\n",
	         (int)start_rc,
	         (int)w1_rc,
	         (int)ilv_inner_rc,
	         (int)ilv_running_after_inner,
	         (int)wb_rc,
	         (int)running_after_b);
	zassert_equal(ilv_inner_rc, ALP_OK);
	zassert_equal(wb_rc, ALP_OK);
	zassert_true(running_after_b, "the writer must resume playback after a concurrent start()");
}

/* issue #2137 review round 2, finding 5 (optional case): force the DROP
 * fallback itself to fail so z_stop()'s both-refused return path is
 * actually reachable in a test, not just reasoned about. */
ZTEST(alp_i2s_start_defer, test_stop_both_drain_and_drop_refused_surfaces_drain_error)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();

	alp_status_t start_rc = alp_i2s_start(h);
	alp_status_t write_rc = write_block(h);
	fake_i2s_tx_simulate_underrun();
	fake_i2s_force_drop_fail(-EIO, 1u);

	alp_status_t stop_rc = alp_i2s_stop(h);

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(start_rc, ALP_OK);
	zassert_equal(write_rc, ALP_OK);
	zassert_equal(stop_rc,
	              ALP_ERR_IO,
	              "both DRAIN (refused -- genuine ERROR state) and DROP (forced) failing must "
	              "surface DRAIN's failure, not silently succeed");
}
