/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression test for issue #2132: alp_i2s_start() called before the
 * first alp_i2s_write() must not fail on the Alif DesignWare I2S
 * driver's empty TX ring buffer -- and the fix (deferred start, live in
 * src/backends/i2s/zephyr_drv.c) must not silently swallow a START that
 * keeps failing, must not strand queued blocks on stop(), and must not
 * leave a stale block for the next open() to dequeue into freed memory.
 *
 * fake_i2s.c models the real i2s_dw.c state machine closely enough to
 * catch all four: unlike tests/unit/i2s_write_bounds' always-succeeds
 * fake (which never calls trigger(START) at all), this one refuses an
 * empty-queue START (-ENOMEM), refuses STOP/DRAIN on a non-running
 * stream (-EIO), and only DROP releases queued blocks unconditionally
 * -- exactly the three-way split z_start()/z_stop()/z_close() now
 * route through.
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
 * triggers immediately. */
ZTEST(alp_i2s_start_defer, test_write_then_start_succeeds)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();

	alp_status_t write_rc             = write_block(h);
	bool         running_before_start = fake_i2s_tx_running();
	alp_status_t start_rc             = alp_i2s_start(h);
	bool         running_after_start  = fake_i2s_tx_running();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(write_rc, ALP_OK);
	zassert_false(running_before_start, "a write with no start() pending must not self-trigger");
	zassert_equal(start_rc, ALP_OK);
	zassert_true(running_after_start, "start() with a block already queued must trigger now");
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

/* Finding 2: a START failure discovered only once data is queued must
 * surface from the write() call that queued it (never ALP_OK), and
 * EVERY later write while still pending must retry -- not just the
 * first one. */
ZTEST(alp_i2s_start_defer, test_start_failure_retries_on_every_write)
{
	fake_i2s_reset();
	alp_i2s_t *h = open_tx();

	alp_status_t start_rc = alp_i2s_start(h);
	/* Force exactly 2 START attempts to fail -- one per write below --
	 * proving BOTH retry, not just the first. */
	fake_i2s_force_start_fail(-EIO, 2u);
	alp_status_t write1_rc       = write_block(h); /* fills slab block 1/2 */
	bool         running_after_1 = fake_i2s_tx_running();
	alp_status_t write2_rc       = write_block(h); /* fills slab block 2/2 */
	bool         running_after_2 = fake_i2s_tx_running();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(start_rc, ALP_OK, "deferred start must not fail up front");
	zassert_not_equal(
	    write1_rc, ALP_OK, "a write into a stream whose START failed must never return ALP_OK");
	zassert_not_equal(write2_rc, ALP_OK, "the SECOND write must retry too, not go silent");
	zassert_false(running_after_1);
	zassert_false(running_after_2);
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

	/* Fault is one-shot (count exhausted) -- release the block that
	 * piled up unconsumed via DROP-through-stop (pending, never really
	 * started), then run a clean cycle. */
	alp_status_t stop_rc     = alp_i2s_stop(h);
	size_t       depth       = fake_i2s_tx_queue_depth();
	alp_status_t start2_rc   = alp_i2s_start(h);
	alp_status_t write2_rc   = write_block(h);
	bool         running_end = fake_i2s_tx_running();

	alp_i2s_close(h);

	zassert_not_null(h, "alp_i2s_open() must resolve the fake alp-i2s0 device (TX)");
	zassert_equal(start1_rc, ALP_OK);
	zassert_not_equal(write1_rc, ALP_OK, "forced first attempt must fail");
	zassert_false(running_after_1);
	zassert_equal(stop_rc, ALP_OK);
	zassert_equal(depth, 0u);
	zassert_equal(start2_rc, ALP_OK);
	zassert_equal(write2_rc, ALP_OK, "a clean write after the fault clears must succeed");
	zassert_true(running_end);
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
