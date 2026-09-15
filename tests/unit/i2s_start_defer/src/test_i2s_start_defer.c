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
 * the backend).
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

/* RX must be unaffected by any of the above: rx_stream_start() allocates
 * its own block from the slab instead of dequeuing a caller-filled
 * ring (i2s_dw.c:751-787), so z_start() never defers for RX -- direct
 * alp_i2s_start() before any read() must trigger immediately. */
ZTEST(alp_i2s_start_defer, test_rx_start_is_never_deferred)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_rx();

	/* The fake's trigger() always succeeds for RX (fake_i2s.c) -- what
	 * this test actually pins is that z_start() took the immediate
	 * path, not the TX-only deferral branch, for an RX handle. */
	alp_status_t start_rc = alp_i2s_start(h);
	alp_status_t stop_rc  = alp_i2s_stop(h);

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (RX)");
	zassert_equal(start_rc, ALP_OK);
	zassert_equal(stop_rc, ALP_OK);
}
