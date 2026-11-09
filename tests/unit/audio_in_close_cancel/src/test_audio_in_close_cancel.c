/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * The real Zephyr audio backend (src/backends/audio/zephyr_drv.c): an
 * alp_audio_in_read() blocked in dmic_read() must be woken by a concurrent
 * alp_audio_in_close() with ALP_ERR_NOT_READY (issue #2977 item 3).
 * dmic_read() has no cancel API, so the backend waits in short slices and
 * re-checks the handle's closing flag (src/common/alp_sliced_wait.h); the
 * fake DMIC (src/fake_dmic.c) honours each slice's timeout and never
 * delivers data.
 *
 * Before the fix the read below (UINT32_MAX = wait forever) never returned
 * and alp_audio_in_close() waited for it forever, so the close runs in its
 * own thread and the test joins it with a bound.
 */
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <alp/audio.h>

#define STACK_SZ 2048
static K_THREAD_STACK_DEFINE(waiter_stack, STACK_SZ);
static K_THREAD_STACK_DEFINE(closer_stack, STACK_SZ);
static struct k_thread waiter_thr, closer_thr;

static alp_status_t g_read_rc;

static void waiter(void *in, void *timeout, void *p3)
{
	ARG_UNUSED(p3);
	int16_t b[64];
	size_t  got = 0;
	g_read_rc   = alp_audio_in_read(in, b, ARRAY_SIZE(b), &got, (uint32_t)(uintptr_t)timeout);
}

static void closer(void *in, void *p2, void *p3)
{
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	alp_audio_in_close(in);
}

static void close_wakes_read(uint32_t timeout_ms)
{
	alp_audio_config_t cfg = ALP_AUDIO_CONFIG_DEFAULT(0);
	alp_audio_in_t    *in  = alp_audio_in_open(&cfg);
	zassert_not_null(in, "open must resolve the fake alp-pdm0 device");

	g_read_rc = ALP_ERR_BUSY; /* sentinel */
	k_thread_create(&waiter_thr,
	                waiter_stack,
	                STACK_SZ,
	                waiter,
	                in,
	                (void *)(uintptr_t)timeout_ms,
	                NULL,
	                K_PRIO_PREEMPT(1),
	                0,
	                K_NO_WAIT);
	k_msleep(100); /* let it block in dmic_read() */
	k_thread_create(&closer_thr,
	                closer_stack,
	                STACK_SZ,
	                closer,
	                in,
	                NULL,
	                NULL,
	                K_PRIO_PREEMPT(1),
	                0,
	                K_NO_WAIT);

	int64_t t0 = k_uptime_get();
	zassert_equal(k_thread_join(&closer_thr, K_SECONDS(2)), 0, "close() hung on a blocked read");
	zassert_equal(k_thread_join(&waiter_thr, K_SECONDS(2)), 0, "blocked read was not woken");
	zassert_true(k_uptime_get() - t0 < 500, "wake took longer than a couple of slices");
	zassert_equal(g_read_rc, ALP_ERR_NOT_READY);
}

ZTEST(alp_audio_in_close_cancel, test_close_wakes_forever_read)
{
	close_wakes_read(UINT32_MAX);
}

ZTEST(alp_audio_in_close_cancel, test_close_wakes_long_timeout_read)
{
	close_wakes_read(30000u);
}

ZTEST(alp_audio_in_close_cancel, test_timeout_still_reported)
{
	alp_audio_config_t cfg = ALP_AUDIO_CONFIG_DEFAULT(0);
	alp_audio_in_t    *in  = alp_audio_in_open(&cfg);
	zassert_not_null(in);
	int16_t b[64];
	size_t  got = 0;

	int64_t t0 = k_uptime_get();
	zassert_equal(alp_audio_in_read(in, b, ARRAY_SIZE(b), &got, 120), ALP_ERR_TIMEOUT);
	int64_t dt = k_uptime_get() - t0;
	zassert_true(dt >= 120 && dt < 400, "timeout %d ms off the requested 120", (int)dt);

	/* timeout_ms=0 stays a single non-blocking attempt. */
	zassert_equal(alp_audio_in_read(in, b, ARRAY_SIZE(b), &got, 0), ALP_ERR_TIMEOUT);
	alp_audio_in_close(in);
}

ZTEST_SUITE(alp_audio_in_close_cancel, NULL, NULL, NULL, NULL, NULL);
