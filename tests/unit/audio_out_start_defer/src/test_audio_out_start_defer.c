/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression test for issue #2132 at the <alp/audio.h> composition
 * layer. The deferred-start mechanism itself (empty-queue START defer,
 * retry-on-write, DROP-not-DRAIN on a never-started stop, DROP-on-close)
 * is exhaustively covered directly against <alp/i2s.h> in
 * tests/unit/i2s_start_defer -- this suite instead proves the audio
 * backend's own composition on top of it still works once the fix moved
 * down a layer:
 *
 *   - the plain (unity-volume) write path forwards a deferred-start
 *     trigger failure AND correctly does NOT count that chunk toward
 *     out_frames -- the I2S layer already released the block before
 *     returning the error (src/backends/audio/zephyr_drv.c's
 *     z_out_write(), MAJOR-1 fix in src/backends/i2s/zephyr_drv.c);
 *   - the volume-scaled CHUNKED write loop (still present after the
 *     fix moved to the I2S layer) composes correctly with a deferred
 *     start across multiple sub-writes -- stereo, > 128 frames per
 *     block so the loop runs at least twice, set_volume(4): the exact
 *     shape the silicon repro (examples/aen/aen-i2s-tas2563-probe) used.
 *
 * Every test closes its handle BEFORE any zassert_* -- see
 * tests/unit/i2s_start_defer's file comment for why (pool-exhaustion
 * cascades measured there).
 */

#include <errno.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/audio.h>

#include "fake_i2s.h"

ZTEST_SUITE(alp_audio_out_start_defer, NULL, NULL, NULL, NULL, NULL);

#define MONO_FRAMES 32u

static alp_audio_out_t *open_mono(void)
{
	alp_audio_config_t cfg = {
		.peripheral_id    = 0u,
		.sample_rate_hz   = 16000u,
		.channels         = 1u,
		.format           = ALP_AUDIO_FMT_S16_LE,
		.frames_per_block = MONO_FRAMES,
	};
	return alp_audio_out_open(&cfg);
}

/* Stereo, 200 frames/block: with the chunk buffer's 256 int16 slots
 * split across 2 channels, chunk_frames = 128, so 200 frames needs TWO
 * chunks (128 + 72) -- the exact shape set_volume(4) forced on the
 * silicon repro (examples/aen/aen-i2s-tas2563-probe). */
#define STEREO_FRAMES 200u

static alp_audio_out_t *open_stereo(void)
{
	alp_audio_config_t cfg = {
		.peripheral_id    = 0u,
		.sample_rate_hz   = 48000u,
		.channels         = 2u,
		.format           = ALP_AUDIO_FMT_S16_LE,
		.frames_per_block = STEREO_FRAMES,
	};
	return alp_audio_out_open(&cfg);
}

/* Plain (unity-volume) regression: start-before-write must not fail,
 * and the write that follows must fire the deferred start -- the
 * original #2132 symptom, reproduced through the audio API. */
ZTEST(alp_audio_out_start_defer, test_mono_start_then_write_succeeds)
{
	fake_i2s_reset();
	alp_audio_out_t *out = open_mono();

	alp_status_t start_rc             = alp_audio_out_start(out);
	bool         running_before_write = fake_i2s_tx_running();

	static int16_t block[MONO_FRAMES] = { 0 };
	size_t         out_frames         = SIZE_MAX;
	alp_status_t   write_rc = alp_audio_out_write(out, block, MONO_FRAMES, &out_frames, 100u);
	bool           running_after_write = fake_i2s_tx_running();

	alp_audio_out_close(out);

	zassert_not_null(out, "alp_audio_out_open() must resolve the fake alp-i2s0 device");
	zassert_equal(start_rc, ALP_OK, "deferred start must not fail");
	zassert_false(running_before_write);
	zassert_equal(write_rc, ALP_OK);
	zassert_equal(out_frames, MONO_FRAMES);
	zassert_true(running_after_write, "the first write() must fire the deferred start()");
}

/* MAJOR 1 (issue #2132 review): a deferred-start trigger failure
 * discovered during write() must surface from that write() call (never
 * ALP_OK), and out_frames must NOT report the failed chunk as pushed --
 * the I2S layer DROPs the block it just queued before returning the
 * error (src/backends/i2s/zephyr_drv.c), so nothing was genuinely
 * queued and out_frames stays at whatever alp_audio_out_write() zeroed
 * it to (src/audio_dispatch.c). A follow-up write, once the fault
 * clears, must still succeed -- proving the DROP didn't also wedge
 * tx_pending_start. */
ZTEST(alp_audio_out_start_defer, test_mono_start_failure_does_not_count_toward_out_frames)
{
	fake_i2s_reset();
	alp_audio_out_t *out = open_mono();

	alp_status_t start_rc = alp_audio_out_start(out);
	fake_i2s_force_start_fail(-EIO, 1u);

	static int16_t block[MONO_FRAMES] = { 0 };
	size_t         out_frames1        = SIZE_MAX;
	alp_status_t   write1_rc = alp_audio_out_write(out, block, MONO_FRAMES, &out_frames1, 100u);
	bool           running_after_1 = fake_i2s_tx_running();
	size_t         depth_after_1   = fake_i2s_tx_queue_depth();

	size_t       out_frames2     = SIZE_MAX;
	alp_status_t write2_rc       = alp_audio_out_write(out, block, MONO_FRAMES, &out_frames2, 100u);
	bool         running_after_2 = fake_i2s_tx_running();

	alp_audio_out_close(out);

	zassert_not_null(out, "alp_audio_out_open() must resolve the fake alp-i2s0 device");
	zassert_equal(start_rc, ALP_OK, "deferred start must not fail up front");
	zassert_not_equal(write1_rc, ALP_OK, "a forced START failure must surface from write()");
	zassert_equal(out_frames1,
	              0u,
	              "a write whose deferred-start retry failed must not report ANY frames as "
	              "pushed -- the I2S layer already released the block (include/alp/audio.h)");
	zassert_false(running_after_1);
	zassert_equal(depth_after_1, 0u, "the failed retry must not strand a block in the ring");
	zassert_equal(write2_rc, ALP_OK, "the next write must retry and succeed once the fault clears");
	zassert_equal(out_frames2, MONO_FRAMES);
	zassert_true(running_after_2);
}

/* Stereo/chunked/set_volume(4), start-before-write: the multi-iteration
 * volume-scaled loop (src/backends/audio/zephyr_drv.c) must compose
 * correctly with a deferred start -- the deferred trigger fires on the
 * FIRST chunk's queue and the SECOND chunk must see it already running,
 * not attempt to defer again. */
ZTEST(alp_audio_out_start_defer, test_stereo_chunked_volume_start_then_write_succeeds)
{
	fake_i2s_reset();
	alp_audio_out_t *out = open_stereo();

	alp_status_t set_vol_rc = alp_audio_out_set_volume(out, 4u); /* matches the silicon repro */
	alp_status_t start_rc   = alp_audio_out_start(out);
	bool         running_before_write = fake_i2s_tx_running();

	static int16_t block[STEREO_FRAMES * 2u]; /* L,R interleaved */
	for (size_t i = 0; i < STEREO_FRAMES * 2u; ++i)
		block[i] = (int16_t)(i & 0xFFu);
	size_t       out_frames = SIZE_MAX;
	alp_status_t write_rc   = alp_audio_out_write(out, block, STEREO_FRAMES, &out_frames, 100u);
	bool         running_after_write = fake_i2s_tx_running();
	/* Exactly ONE trigger(START) must have fired across both chunks --
	 * the first chunk's queue fires it (tx_pending_start cleared there);
	 * the second chunk must see it already clear and skip the retry
	 * entirely. A second (redundant) trigger attempt would hit the
	 * fake's already-running -EIO and fail the write above, so
	 * write_rc == ALP_OK already implies this -- this depth check is
	 * the independent, fake-bookkeeping-based confirmation: the fake
	 * only frees a ring entry when a trigger actually consumes it, so
	 * chunk 2 sitting un-freed here (depth 1, not 2 and not 0) proves
	 * chunk 1 was freed by exactly one trigger and chunk 2 by none. */
	size_t depth_after = fake_i2s_tx_queue_depth();

	alp_audio_out_close(out);

	zassert_not_null(out, "alp_audio_out_open() must resolve the fake alp-i2s0 device");
	zassert_equal(set_vol_rc, ALP_OK);
	zassert_equal(start_rc, ALP_OK, "deferred start must not fail");
	zassert_false(running_before_write);
	zassert_equal(write_rc, ALP_OK, "the chunked write must fully succeed");
	zassert_equal(out_frames, STEREO_FRAMES, "all frames across both chunks must be reported");
	zassert_true(running_after_write, "the first chunk must have fired the deferred start()");
	zassert_equal(depth_after, 1u, "chunk 1 consumed by START, chunk 2 left queued behind it");
}

/* Same stereo/chunked/set_volume(4) shape, write-before-start: proves
 * the chunk loop doesn't regress the already-queued immediate-start
 * path either. */
ZTEST(alp_audio_out_start_defer, test_stereo_chunked_volume_write_then_start_succeeds)
{
	fake_i2s_reset();
	alp_audio_out_t *out = open_stereo();

	alp_status_t set_vol_rc = alp_audio_out_set_volume(out, 4u);

	static int16_t block[STEREO_FRAMES * 2u];
	for (size_t i = 0; i < STEREO_FRAMES * 2u; ++i)
		block[i] = (int16_t)(i & 0xFFu);
	size_t       out_frames = SIZE_MAX;
	alp_status_t write_rc   = alp_audio_out_write(out, block, STEREO_FRAMES, &out_frames, 100u);
	bool         running_before_start = fake_i2s_tx_running();

	alp_status_t start_rc            = alp_audio_out_start(out);
	bool         running_after_start = fake_i2s_tx_running();

	alp_audio_out_close(out);

	zassert_not_null(out, "alp_audio_out_open() must resolve the fake alp-i2s0 device");
	zassert_equal(set_vol_rc, ALP_OK);
	zassert_equal(write_rc, ALP_OK);
	zassert_equal(out_frames, STEREO_FRAMES);
	zassert_false(running_before_start, "queuing alone must not self-trigger");
	zassert_equal(start_rc, ALP_OK);
	zassert_true(running_after_start, "start() with blocks already queued must trigger now");
}

/* issue #2137 review round 2, finding 5: the underrun-recovery sequence
 * exercised directly against <alp/i2s.h> in tests/unit/i2s_start_defer
 * must also compose correctly through the <alp/audio.h> layer --
 * silicon repro: both alp_audio_out_stop(I2S3) and alp_audio_out_start()
 * returned -5 after the queue ran dry, e1m-aen-evk-03. */
ZTEST(alp_audio_out_start_defer, test_mono_underrun_stop_start_write_recovers)
{
	fake_i2s_reset();
	alp_audio_out_t *out = open_mono();

	alp_status_t start_rc = alp_audio_out_start(out);

	static int16_t block[MONO_FRAMES] = { 0 };
	size_t         out_frames1        = SIZE_MAX;
	alp_status_t   write1_rc = alp_audio_out_write(out, block, MONO_FRAMES, &out_frames1, 100u);
	bool           running_after_write1 = fake_i2s_tx_running();

	fake_i2s_tx_simulate_underrun();
	bool in_error_after_underrun = fake_i2s_tx_in_error();

	alp_status_t stop_rc            = alp_audio_out_stop(out);
	bool         running_after_stop = fake_i2s_tx_running();

	alp_status_t start2_rc = alp_audio_out_start(out);

	size_t       out_frames2 = SIZE_MAX;
	alp_status_t write2_rc   = alp_audio_out_write(out, block, MONO_FRAMES, &out_frames2, 100u);
	bool         running_after_write2 = fake_i2s_tx_running();

	alp_audio_out_close(out);

	zassert_not_null(out, "alp_audio_out_open() must resolve the fake alp-i2s0 device");
	zassert_equal(start_rc, ALP_OK);
	zassert_equal(write1_rc, ALP_OK);
	zassert_equal(out_frames1, MONO_FRAMES);
	zassert_true(running_after_write1);
	zassert_true(in_error_after_underrun, "the underrun must have reached I2S_STATE_ERROR");
	zassert_equal(stop_rc,
	              ALP_OK,
	              "stop() after an underrun must recover through the audio composition layer "
	              "too, not just <alp/i2s.h> directly");
	zassert_false(running_after_stop);
	zassert_equal(start2_rc, ALP_OK, "start() after the recovery stop must succeed");
	zassert_equal(write2_rc, ALP_OK, "write() must resume playback");
	zassert_equal(out_frames2, MONO_FRAMES);
	zassert_true(running_after_write2);
}
