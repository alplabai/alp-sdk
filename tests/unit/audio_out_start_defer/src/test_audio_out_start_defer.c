/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression test for issue #2132: alp_audio_out_start() called before
 * the first alp_audio_out_write() must not fail ALP_ERR_NOMEM on the
 * Alif DesignWare I2S driver's empty TX ring buffer.
 *
 * fake_i2s.c models the ONE trap that let this ship: unlike
 * tests/unit/i2s_write_bounds' always-succeeds fake, this one refuses a
 * TX I2S_TRIGGER_START with -ENOMEM when nothing has been queued
 * (mirrors zephyr/drivers/i2s/i2s_dw.c's queue_get(), i2s_dw.c:144-147),
 * and refuses STOP/DRAIN on a stream that never reached RUNNING
 * (mirrors i2s_dw.c:301-304 / :315-321). Both are exactly what a
 * NULL/always-succeeds fake -- and native_sim, which has no I2S device
 * at all -- cannot catch.
 *
 * Every test closes its handle right after the last call that needs it
 * and BEFORE any zassert_* that could longjmp out: CONFIG_ALP_SDK_MAX_
 * AUDIO_OUT_HANDLES defaults to 1, so a failing assertion that skipped
 * the close would leak the single pool slot and cascade into every
 * later test's open_out() -- mirrors tests/zephyr/chips/src/test_audio.c's
 * close-before-assert convention (see tas_init()'s comment there).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/audio.h>

#include "fake_i2s.h"

ZTEST_SUITE(alp_audio_out_start_defer, NULL, NULL, NULL, NULL, NULL);

#define FRAMES_PER_BLOCK 32u

static alp_audio_out_t *open_out(void)
{
	alp_audio_config_t cfg = {
		.peripheral_id    = 0u,
		.sample_rate_hz   = 16000u,
		.channels         = 1u,
		.format           = ALP_AUDIO_FMT_S16_LE,
		.frames_per_block = FRAMES_PER_BLOCK,
	};
	return alp_audio_out_open(&cfg);
}

/* The core regression: start() before the first write() must return
 * ALP_OK immediately (the real I2S trigger is deferred), and the write
 * that follows must be the call that actually fires it. */
ZTEST(alp_audio_out_start_defer, test_start_then_write_succeeds)
{
	fake_i2s_reset();
	alp_audio_out_t *out = open_out();

	/* Pre-fix: this hit i2s_dw.c's empty-queue -ENOMEM immediately and
	 * returned ALP_ERR_NOMEM here -- issue #2132. */
	alp_status_t start_rc             = alp_audio_out_start(out);
	bool         running_before_write = fake_i2s_tx_running();

	static int16_t block[FRAMES_PER_BLOCK] = { 0 };
	size_t         out_frames              = SIZE_MAX;
	alp_status_t   write_rc = alp_audio_out_write(out, block, FRAMES_PER_BLOCK, &out_frames, 100u);
	bool           running_after_write = fake_i2s_tx_running();

	alp_audio_out_close(out);

	zassert_not_null(out, "alp_audio_out_open() must resolve the fake alp-i2s0 device");
	zassert_equal(start_rc, ALP_OK, "deferred start must not fail");
	zassert_false(running_before_write,
	              "start() with nothing queued must NOT have triggered the real I2S START yet");
	zassert_equal(write_rc, ALP_OK);
	zassert_equal(out_frames, FRAMES_PER_BLOCK);
	zassert_true(running_after_write, "the first write() must have fired the deferred start()");
}

/* Write-then-start (the order every in-tree caller used to need) must
 * keep working unchanged: the block is already queued, so start()
 * triggers immediately. */
ZTEST(alp_audio_out_start_defer, test_write_then_start_succeeds)
{
	fake_i2s_reset();
	alp_audio_out_t *out = open_out();

	static int16_t block[FRAMES_PER_BLOCK] = { 0 };
	size_t         out_frames              = SIZE_MAX;
	alp_status_t   write_rc = alp_audio_out_write(out, block, FRAMES_PER_BLOCK, &out_frames, 100u);
	bool           running_before_start = fake_i2s_tx_running();

	alp_status_t start_rc            = alp_audio_out_start(out);
	bool         running_after_start = fake_i2s_tx_running();

	alp_audio_out_close(out);

	zassert_not_null(out, "alp_audio_out_open() must resolve the fake alp-i2s0 device");
	zassert_equal(write_rc, ALP_OK);
	zassert_equal(out_frames, FRAMES_PER_BLOCK);
	zassert_false(running_before_start, "a write with no start() pending must not self-trigger");
	zassert_equal(start_rc, ALP_OK);
	zassert_true(running_after_start, "start() with a block already queued must trigger now");
}

/* Stopping a pending-but-never-written stream must not issue a real I2S
 * trigger -- i2s_dw.c's STOP/DRAIN refuse a stream that never reached
 * RUNNING (-EIO), so if z_out_stop() forwarded to it, this would fail. */
ZTEST(alp_audio_out_start_defer, test_start_then_stop_with_no_write_is_ok)
{
	fake_i2s_reset();
	alp_audio_out_t *out = open_out();

	alp_status_t start_rc = alp_audio_out_start(out);
	alp_status_t stop_rc  = alp_audio_out_stop(out);
	bool         running  = fake_i2s_tx_running();

	alp_audio_out_close(out);

	zassert_not_null(out, "alp_audio_out_open() must resolve the fake alp-i2s0 device");
	zassert_equal(start_rc, ALP_OK);
	zassert_equal(stop_rc,
	              ALP_OK,
	              "stopping a never-started (pending) stream must not surface the fake's "
	              "not-running -EIO");
	zassert_false(running);
}

/* A start trigger failure discovered only once data is queued must
 * surface from the write() call that queued it, not be swallowed. */
ZTEST(alp_audio_out_start_defer, test_start_failure_on_first_write_surfaces)
{
	fake_i2s_reset();
	alp_audio_out_t *out = open_out();

	alp_status_t start_rc = alp_audio_out_start(out);

	fake_i2s_force_start_fail(-EIO);
	static int16_t block[FRAMES_PER_BLOCK] = { 0 };
	size_t         out_frames              = SIZE_MAX;
	alp_status_t   write_rc = alp_audio_out_write(out, block, FRAMES_PER_BLOCK, &out_frames, 100u);
	bool           running  = fake_i2s_tx_running();

	alp_audio_out_close(out);

	zassert_not_null(out, "alp_audio_out_open() must resolve the fake alp-i2s0 device");
	zassert_equal(start_rc, ALP_OK, "deferred start must not fail up front");
	zassert_equal(write_rc, ALP_ERR_IO, "the forced START failure must surface from write()");
	zassert_equal(out_frames,
	              FRAMES_PER_BLOCK,
	              "the write itself queued fine -- only the deferred start trigger failed");
	zassert_false(running, "the forced START failure must not have gone RUNNING");
}
