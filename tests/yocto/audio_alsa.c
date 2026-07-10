/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plain-CMake tests for the Yocto/ALSA audio backend
 * (src/backends/audio/yocto_drv.c).
 *
 * Failure-path coverage only -- happy paths require a working
 * ALSA configuration on the runner (real device or dummy driver),
 * which is parked behind docs/ci/HW-IN-LOOP.md.  The tests below
 * exercise argument validation + the resolve_device_name +
 * configure_pcm error paths, plus #632's hermetic (real-device-free)
 * coverage of the allocation-free chunked write core and the shared
 * timeout-conversion helper.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_audio_alsa
 *   ctest --test-dir build -R alp_test_audio_alsa
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include <alsa/asoundlib.h>

#include "alp/audio.h"
#include "alp/peripheral.h"

#include "test_assert.h"

/* ------------------------------------------------------------------ */
/* #632 -- internal seams from src/backends/audio/yocto_drv.c.  Not    */
/* declared in any public header; same pattern as                     */
/* alp_uart_read_fd_bounded in tests/yocto/peripheral_uart.c.          */
/* ------------------------------------------------------------------ */

/* Portable timeout_ms -> ALSA snd_pcm_wait() argument. */
extern int alp_yocto_alsa_wait_arg(uint32_t timeout_ms);

/* Allocation-free chunked write core.  Parameterised over the
 * low-level ALSA wait/write calls so it can be driven against an
 * in-memory fake PCM with no real ALSA device.  `pcm` is passed
 * through opaque to wait_fn/writei_fn -- never dereferenced by this
 * function itself -- so a fake, never-NULL sentinel pointer is safe. */
extern alp_status_t alp_yocto_alsa_out_write_core(
    snd_pcm_t         *pcm,
    alp_audio_format_t format,
    uint8_t            channels,
    uint8_t            sample_bytes,
    uint8_t            volume,
    uint8_t           *scratch,
    uint16_t           scratch_frames,
    const void        *buf,
    size_t             frames,
    size_t            *out_frames,
    uint32_t           timeout_ms,
    int (*wait_fn)(snd_pcm_t *pcm, int timeout_ms),
    snd_pcm_sframes_t (*writei_fn)(snd_pcm_t *pcm, const void *buf, snd_pcm_uframes_t frames));

/* Opaque, never-dereferenced sentinel standing in for a real snd_pcm_t
 * -- the fake wait/writei below never touch it, they just record what
 * they were asked to do. */
static snd_pcm_t *const FAKE_PCM = (snd_pcm_t *)(uintptr_t)0x1;

/* CI runners don't generally have ALSA devices wired up; the
 * default "default" device exists in /etc/asound.conf form on
 * ubuntu-latest but PCM-open against it fails cleanly to NOT_READY
 * or IO depending on the runner.  We assert "open refuses" rather
 * than a specific status code so the test passes on any
 * deviceless host. */

static void test_in_null_cfg_returns_null(void)
{
	alp_audio_in_t *h = alp_audio_in_open(NULL);
	ALP_ASSERT_NULL(h);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_out_null_cfg_returns_null(void)
{
	alp_audio_out_t *h = alp_audio_out_open(NULL);
	ALP_ASSERT_NULL(h);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_in_invalid_format_returns_null(void)
{
	alp_audio_config_t cfg = {
		.peripheral_id    = 0,
		.sample_rate_hz   = 16000,
		.channels         = 1,
		.format           = (alp_audio_format_t)99, /* not a valid format */
		.frames_per_block = 256,
	};
	alp_audio_in_t *h = alp_audio_in_open(&cfg);
	ALP_ASSERT_NULL(h);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_out_invalid_format_returns_null(void)
{
	alp_audio_config_t cfg = {
		.peripheral_id    = 0,
		.sample_rate_hz   = 48000,
		.channels         = 2,
		.format           = (alp_audio_format_t)99,
		.frames_per_block = 256,
	};
	alp_audio_out_t *h = alp_audio_out_open(&cfg);
	ALP_ASSERT_NULL(h);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_in_unreachable_device_refuses(void)
{
	/* peripheral_id = 999 => "hw:998,0", which never exists. ALSA
     * returns -ENOENT / -ENODEV; we map to NOT_READY. */
	alp_audio_config_t cfg = {
		.peripheral_id    = 999,
		.sample_rate_hz   = 16000,
		.channels         = 1,
		.format           = ALP_AUDIO_FMT_S16_LE,
		.frames_per_block = 256,
	};
	alp_audio_in_t *h = alp_audio_in_open(&cfg);
	ALP_ASSERT_NULL(h);
	/* Either NOT_READY (device missing) or IO (driver failure) is
     * acceptable -- we just assert "open refused". */
	ALP_ASSERT_TRUE(alp_last_error() != ALP_OK);
}

static void test_start_on_null_returns_not_ready(void)
{
	ALP_ASSERT_EQ_INT(alp_audio_in_start(NULL), ALP_ERR_NOT_READY);
	ALP_ASSERT_EQ_INT(alp_audio_out_start(NULL), ALP_ERR_NOT_READY);
}

static void test_stop_on_null_returns_not_ready(void)
{
	ALP_ASSERT_EQ_INT(alp_audio_in_stop(NULL), ALP_ERR_NOT_READY);
	ALP_ASSERT_EQ_INT(alp_audio_out_stop(NULL), ALP_ERR_NOT_READY);
}

static void test_read_on_null_returns_not_ready(void)
{
	uint8_t      buf[64];
	size_t       got = 99;
	alp_status_t s   = alp_audio_in_read(NULL, buf, sizeof(buf), &got, 100);
	ALP_ASSERT_EQ_INT(s, ALP_ERR_NOT_READY);
	ALP_ASSERT_EQ_INT(got, 0);
}

static void test_write_on_null_returns_not_ready(void)
{
	const uint8_t payload[] = "abcdef";
	size_t        wrote     = 99;
	alp_status_t  s         = alp_audio_out_write(NULL, payload, sizeof(payload), &wrote, 100);
	ALP_ASSERT_EQ_INT(s, ALP_ERR_NOT_READY);
	ALP_ASSERT_EQ_INT(wrote, 0);
}

static void test_set_volume_on_null_returns_not_ready(void)
{
	ALP_ASSERT_EQ_INT(alp_audio_out_set_volume(NULL, 128), ALP_ERR_NOT_READY);
}

static void test_close_null_is_safe(void)
{
	alp_audio_in_close(NULL);
	alp_audio_out_close(NULL);
	ALP_TEST_PASS();
}

/* ---------- #632: timeout conversion (alp_yocto_alsa_wait_arg) ---------- */

static void test_timeout_zero_maps_to_zero(void)
{
	ALP_ASSERT_EQ_INT(alp_yocto_alsa_wait_arg(0u), 0);
}

static void test_timeout_finite_passes_through(void)
{
	ALP_ASSERT_EQ_INT(alp_yocto_alsa_wait_arg(250u), 250);
}

static void test_timeout_int_max_passes_through(void)
{
	ALP_ASSERT_EQ_INT(alp_yocto_alsa_wait_arg((uint32_t)INT_MAX), INT_MAX);
}

static void test_timeout_int_max_plus_one_clamps_not_wraps(void)
{
	/* Before the fix, a plain (int)timeout_ms cast wrapped
     * (uint32_t)INT_MAX + 1 into a NEGATIVE int -- which ALSA reads
     * as "wait forever" -- for a caller that asked for a large but
     * finite wait.  It must clamp to INT_MAX (still finite, still
     * positive), never go negative. */
	uint32_t timeout_ms = (uint32_t)INT_MAX + 1u;
	int      got        = alp_yocto_alsa_wait_arg(timeout_ms);
	ALP_ASSERT_TRUE(got > 0);
	ALP_ASSERT_EQ_INT(got, INT_MAX);
}

static void test_timeout_uint32_max_means_forever(void)
{
	/* UINT32_MAX is the SDK-wide "block forever" sentinel (see
     * include/alp/peripheral.h) -- ALSA's forever value is -1. */
	ALP_ASSERT_EQ_INT(alp_yocto_alsa_wait_arg(UINT32_MAX), -1);
}

/* ---------- #632: allocation-free chunked write core ---------- */

/* Fakes for alp_yocto_alsa_out_write_core's wait_fn / writei_fn.  A
 * single-threaded test binary, run sequentially -- plain globals reset
 * at the top of each test are fine. */
#define FAKE_MAX_CALLS 16

static int      g_wait_calls;
static int      g_wait_ms_seen[FAKE_MAX_CALLS];
static int      g_wait_return;           /* 1 = ready, 0 = timeout, <0 = error */
static uint32_t g_wait_sleep_ms_on_call; /* simulate a slow chunk once */
static int      g_wait_sleep_on_call_index;

/* writei fakes: capture what was written (pointer + first bytes) so
 * tests can assert zero-copy (pointer identity) or scaled contents. */
static int               g_writei_calls;
static const void       *g_writei_ptr_seen[FAKE_MAX_CALLS];
static snd_pcm_uframes_t g_writei_frames_seen[FAKE_MAX_CALLS];
static bool              g_writei_partial_first_call;

/* Resets EVERY fake global -- both wait_fn and writei_fn state --
 * so tests stay independent of run order (a stale g_writei_calls or
 * g_writei_partial_first_call from a previous test would silently
 * corrupt the next test's chunk/call-count assertions). */
static void reset_fakes(void)
{
	g_wait_calls = 0;
	memset(g_wait_ms_seen, 0, sizeof(g_wait_ms_seen));
	g_wait_return              = 1;
	g_wait_sleep_ms_on_call    = 0;
	g_wait_sleep_on_call_index = -1;

	g_writei_calls = 0;
	memset(g_writei_ptr_seen, 0, sizeof(g_writei_ptr_seen));
	memset(g_writei_frames_seen, 0, sizeof(g_writei_frames_seen));
	g_writei_partial_first_call = false;
}

static int fake_wait(snd_pcm_t *pcm, int timeout_ms)
{
	ALP_ASSERT_TRUE(pcm == FAKE_PCM);
	if (g_wait_calls < FAKE_MAX_CALLS) g_wait_ms_seen[g_wait_calls] = timeout_ms;
	if (g_wait_calls == g_wait_sleep_on_call_index && g_wait_sleep_ms_on_call > 0) {
		struct timespec req = {
			.tv_sec  = g_wait_sleep_ms_on_call / 1000u,
			.tv_nsec = (long)(g_wait_sleep_ms_on_call % 1000u) * 1000000L,
		};
		nanosleep(&req, NULL);
	}
	++g_wait_calls;
	return g_wait_return;
}

static snd_pcm_sframes_t fake_writei(snd_pcm_t *pcm, const void *buf, snd_pcm_uframes_t frames)
{
	ALP_ASSERT_TRUE(pcm == FAKE_PCM);
	if (g_writei_calls < FAKE_MAX_CALLS) {
		g_writei_ptr_seen[g_writei_calls]    = buf;
		g_writei_frames_seen[g_writei_calls] = frames;
	}
	/* Accept everything asked for, unless this test wants the FIRST
     * call only to simulate a partial ALSA write. */
	snd_pcm_sframes_t accepted = (snd_pcm_sframes_t)frames;
	if (g_writei_partial_first_call && g_writei_calls == 0 && accepted > 1) {
		accepted = accepted / 2; /* simulate a partial ALSA write */
	}
	++g_writei_calls;
	return accepted;
}

static void test_write_core_unity_volume_is_zero_copy_single_chunk(void)
{
	reset_fakes();

	int16_t src[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t scratch[4 * 2 * 2]; /* unused on this path -- volume is full */

	size_t       out_frames = 99;
	alp_status_t s          = alp_yocto_alsa_out_write_core(FAKE_PCM,
	                                                        ALP_AUDIO_FMT_S16_LE,
	                                                        2 /* channels */,
	                                                        2 /* sample_bytes */,
	                                                        255 /* volume */,
	                                                        scratch,
	                                                        4 /* scratch_frames */,
	                                                        src,
	                                                        4 /* frames (== 8 int16 samples) */,
	                                                        &out_frames,
	                                                        100u,
	                                                        fake_wait,
	                                                        fake_writei);

	ALP_ASSERT_EQ_INT(s, ALP_OK);
	ALP_ASSERT_EQ_INT(out_frames, 4);
	ALP_ASSERT_EQ_INT(g_writei_calls, 1);         /* one chunk -- no scratch needed */
	ALP_ASSERT_TRUE(g_writei_ptr_seen[0] == src); /* zero-copy: caller's buffer, not scratch */
}

static void test_write_core_scaled_write_processed_in_bounded_chunks(void)
{
	reset_fakes();

	/* scratch holds 4 frames * 2 channels; ask for 10 frames of
     * scaled volume -- must split into ceil(10/4) = 3 bounded chunks,
     * each writing FROM the scratch buffer (never straight from src),
     * and each chunk's frame count must never exceed scratch_frames. */
	const uint16_t scratch_frames = 4;
	int16_t        src[10 * 2];
	for (int i = 0; i < 20; ++i)
		src[i] = (int16_t)(1000 + i);
	uint8_t scratch[4 * 2 * 2];

	size_t       out_frames = 0;
	alp_status_t s = alp_yocto_alsa_out_write_core(FAKE_PCM,
	                                               ALP_AUDIO_FMT_S16_LE,
	                                               2,
	                                               2,
	                                               128 /* not full volume -- scaling engaged */,
	                                               scratch,
	                                               scratch_frames,
	                                               src,
	                                               10,
	                                               &out_frames,
	                                               100u,
	                                               fake_wait,
	                                               fake_writei);

	ALP_ASSERT_EQ_INT(s, ALP_OK);
	ALP_ASSERT_EQ_INT(out_frames, 10);
	ALP_ASSERT_EQ_INT(g_writei_calls, 3); /* 4 + 4 + 2 */
	for (int i = 0; i < g_writei_calls; ++i) {
		ALP_ASSERT_TRUE(g_writei_ptr_seen[i] == (const void *)scratch);
		ALP_ASSERT_TRUE(g_writei_frames_seen[i] <= scratch_frames);
	}
	ALP_ASSERT_EQ_INT(g_writei_frames_seen[0], 4);
	ALP_ASSERT_EQ_INT(g_writei_frames_seen[1], 4);
	ALP_ASSERT_EQ_INT(g_writei_frames_seen[2], 2);
}

static void test_write_core_partial_alsa_write_accumulates_out_frames(void)
{
	reset_fakes();
	g_writei_partial_first_call = true; /* first writei() call only accepts half */

	int16_t src[6 * 2];
	for (int i = 0; i < 12; ++i)
		src[i] = (int16_t)(i);

	size_t       out_frames = 0;
	alp_status_t s          = alp_yocto_alsa_out_write_core(FAKE_PCM,
	                                                        ALP_AUDIO_FMT_S16_LE,
	                                                        2,
	                                                        2,
	                                                        255 /* unity -- exercise the ALSA
	                                                               partial-write path, not the
	                                                               scratch chunking path */
	                                                        ,
	                                                        NULL,
	                                                        0,
	                                                        src,
	                                                        6,
	                                                        &out_frames,
	                                                        100u,
	                                                        fake_wait,
	                                                        fake_writei);

	ALP_ASSERT_EQ_INT(s, ALP_OK);
	/* First call asked for 6, ALSA accepted 3 (half); second call asks
     * for the remaining 3 and accepts all 3 -- total must be 6, not 3. */
	ALP_ASSERT_EQ_INT(out_frames, 6);
	ALP_ASSERT_EQ_INT(g_writei_calls, 2);
	ALP_ASSERT_EQ_INT(g_writei_frames_seen[0], 6);
	ALP_ASSERT_EQ_INT(g_writei_frames_seen[1], 3);
}

static void test_write_core_zero_timeout_is_single_nonblocking_attempt(void)
{
	reset_fakes();
	g_wait_return = 0; /* never ready */

	int16_t      src[4]     = { 1, 2, 3, 4 };
	size_t       out_frames = 99;
	alp_status_t s          = alp_yocto_alsa_out_write_core(FAKE_PCM,
	                                                        ALP_AUDIO_FMT_S16_LE,
	                                                        1,
	                                                        2,
	                                                        255,
	                                                        NULL,
	                                                        0,
	                                                        src,
	                                                        4,
	                                                        &out_frames,
	                                                        0u /* don't wait at all */,
	                                                        fake_wait,
	                                                        fake_writei);

	ALP_ASSERT_EQ_INT(s, ALP_ERR_TIMEOUT);
	ALP_ASSERT_EQ_INT(out_frames, 0);
	ALP_ASSERT_EQ_INT(g_wait_calls, 1);
	ALP_ASSERT_EQ_INT(g_wait_ms_seen[0], 0);
	ALP_ASSERT_EQ_INT(g_writei_calls, 0); /* never got to write */
}

static void test_write_core_uint32_max_timeout_waits_forever_arg(void)
{
	reset_fakes();

	int16_t      src[4]     = { 1, 2, 3, 4 };
	size_t       out_frames = 0;
	alp_status_t s          = alp_yocto_alsa_out_write_core(FAKE_PCM,
	                                                        ALP_AUDIO_FMT_S16_LE,
	                                                        1,
	                                                        2,
	                                                        255,
	                                                        NULL,
	                                                        0,
	                                                        src,
	                                                        4,
	                                                        &out_frames,
	                                                        UINT32_MAX,
	                                                        fake_wait,
	                                                        fake_writei);

	ALP_ASSERT_EQ_INT(s, ALP_OK);
	ALP_ASSERT_EQ_INT(g_wait_ms_seen[0], -1); /* forever, per the portable contract */
}

static void test_write_core_wait_error_propagates(void)
{
	reset_fakes();
	g_wait_return = -EIO;

	int16_t      src[4]     = { 1, 2, 3, 4 };
	size_t       out_frames = 99;
	alp_status_t s          = alp_yocto_alsa_out_write_core(FAKE_PCM,
	                                                        ALP_AUDIO_FMT_S16_LE,
	                                                        1,
	                                                        2,
	                                                        255,
	                                                        NULL,
	                                                        0,
	                                                        src,
	                                                        4,
	                                                        &out_frames,
	                                                        100u,
	                                                        fake_wait,
	                                                        fake_writei);

	ALP_ASSERT_EQ_INT(s, ALP_ERR_IO);
	ALP_ASSERT_EQ_INT(out_frames, 0);
	ALP_ASSERT_EQ_INT(g_writei_calls, 0);
}

static void test_write_core_one_deadline_shared_across_chunks_not_reset_per_chunk(void)
{
	/* The exact #632 shape for the timeout half of the bug: before the
     * fix, a re-derived per-chunk timeout would hand the FULL
     * caller-requested budget to every chunk's wait() call, so N slow
     * chunks could take up to N times the caller's requested bound.
     * Here chunk 1 is slow (simulated via a real sleep inside
     * fake_wait) -- the second chunk's wait_ms argument must reflect
     * the SHRUNK remaining budget, not the original ~200ms again. */
	reset_fakes();
	g_wait_sleep_on_call_index = 0;
	g_wait_sleep_ms_on_call    = 60;

	const uint16_t scratch_frames = 2;
	int16_t        src[4 * 2];
	for (int i = 0; i < 8; ++i)
		src[i] = (int16_t)(100 + i);
	uint8_t scratch[2 * 2 * 2];

	size_t       out_frames = 0;
	alp_status_t s = alp_yocto_alsa_out_write_core(FAKE_PCM,
	                                               ALP_AUDIO_FMT_S16_LE,
	                                               2,
	                                               2,
	                                               128 /* scaling -- forces 2 chunks of 2 frames */,
	                                               scratch,
	                                               scratch_frames,
	                                               src,
	                                               4,
	                                               &out_frames,
	                                               200u,
	                                               fake_wait,
	                                               fake_writei);

	ALP_ASSERT_EQ_INT(s, ALP_OK);
	ALP_ASSERT_EQ_INT(out_frames, 4);
	ALP_ASSERT_EQ_INT(g_wait_calls, 2);
	/* First chunk saw close to the full 200ms budget. */
	ALP_ASSERT_TRUE(g_wait_ms_seen[0] >= 180 && g_wait_ms_seen[0] <= 200);
	/* Second chunk's budget shrank by roughly the 60ms already spent
     * sleeping in the first wait -- nowhere near a fresh 200ms. */
	ALP_ASSERT_TRUE(g_wait_ms_seen[1] > 0 && g_wait_ms_seen[1] <= 160);
}

int main(void)
{
	test_in_null_cfg_returns_null();
	test_out_null_cfg_returns_null();
	test_in_invalid_format_returns_null();
	test_out_invalid_format_returns_null();
	test_in_unreachable_device_refuses();
	test_start_on_null_returns_not_ready();
	test_stop_on_null_returns_not_ready();
	test_read_on_null_returns_not_ready();
	test_write_on_null_returns_not_ready();
	test_set_volume_on_null_returns_not_ready();
	test_close_null_is_safe();

	test_timeout_zero_maps_to_zero();
	test_timeout_finite_passes_through();
	test_timeout_int_max_passes_through();
	test_timeout_int_max_plus_one_clamps_not_wraps();
	test_timeout_uint32_max_means_forever();

	test_write_core_unity_volume_is_zero_copy_single_chunk();
	test_write_core_scaled_write_processed_in_bounded_chunks();
	test_write_core_partial_alsa_write_accumulates_out_frames();
	test_write_core_zero_timeout_is_single_nonblocking_attempt();
	test_write_core_uint32_max_timeout_waits_forever_arg();
	test_write_core_wait_error_propagates();
	test_write_core_one_deadline_shared_across_chunks_not_reset_per_chunk();

	ALP_TEST_SUMMARY();
}
