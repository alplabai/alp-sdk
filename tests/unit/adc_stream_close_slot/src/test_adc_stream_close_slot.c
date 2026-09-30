/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression: alp_adc_stream_close() (and alp_adc_stream_open()'s
 * pool-exhausted rollback) sent STREAM_END only when
 * alp_z_v2n_supervisor_acquire() returned ALP_OK, but freed the host-side
 * stream slot UNCONDITIONALLY.  A busy / timed-out acquire therefore left
 * the GD32 stream running while the slot was handed back, and the next
 * open that reused the slot got STREAM_BEGIN -> STATUS_INVAL forever.
 *
 * The fix retries the acquire (bounded) and frees the slot only once
 * STREAM_END was delivered; when it never is, the slot stays reserved.
 *
 * The real peripheral_adc.c runs against a scripted supervisor and a
 * per-slot GD32 model (src/stubs.c); see CMakeLists.txt for why the module
 * itself is not linked.  The handle pool is sized to ONE (CMakeLists.txt).
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <alp/adc.h>

#include "handles.h"
#include "stubs.h"

static void each_before(void *unused)
{
	(void)unused;
	memset(&g_stub, 0, sizeof(g_stub));
}

ZTEST_SUITE(adc_stream_close_slot, NULL, NULL, each_before, NULL, NULL);

static alp_adc_stream_t *open_ch(uint32_t ch)
{
	return alp_adc_stream_open(
	    &(alp_adc_stream_config_t){ .channel_id = ch, .sample_rate_hz = 1000u });
}

/* Happy path baseline: close ends the GD32 stream and frees the slot. */
ZTEST(adc_stream_close_slot, test_close_ends_stream_and_frees_slot)
{
	alp_adc_stream_t *h = open_ch(0u);
	zassert_not_null(h);
	zassert_true(g_stub.gd32_stream_active[0]);

	alp_adc_stream_close(h);
	zassert_false(g_stub.gd32_stream_active[0]);

	h = open_ch(0u);
	zassert_not_null(h, "slot 0 must be reusable after a clean close");
	alp_adc_stream_close(h);
}

/* A transiently busy supervisor must not skip STREAM_END: the bounded retry
 * gets it through, so the slot is genuinely free again. */
ZTEST(adc_stream_close_slot, test_close_retries_busy_acquire)
{
	alp_adc_stream_t *h = open_ch(0u);
	zassert_not_null(h);

	g_stub.script[0]  = ALP_ERR_BUSY;
	g_stub.script[1]  = ALP_ERR_BUSY;
	g_stub.script_len = 2;
	g_stub.script_pos = 0;
	alp_adc_stream_close(h);

	zassert_false(g_stub.gd32_stream_active[0], "STREAM_END skipped on a busy acquire");
	h = open_ch(0u);
	zassert_not_null(h, "slot 0 reopen failed: last_error=%d", g_stub.last_error);
	alp_adc_stream_close(h);
}

/* Open rollback: STREAM_BEGIN succeeded but the handle pool is exhausted, so
 * the stream is rolled back.  A busy supervisor on the first rollback
 * acquire must not leak the GD32 stream. */
ZTEST(adc_stream_close_slot, test_open_rollback_retries_busy_acquire)
{
	alp_adc_stream_t *held = open_ch(0u); /* takes the single pool handle */
	zassert_not_null(held);

	/* open(): begin-acquire OK, then the rollback acquire is busy once. */
	g_stub.script[0]  = ALP_OK;
	g_stub.script[1]  = ALP_ERR_BUSY;
	g_stub.script_len = 2;
	g_stub.script_pos = 0;

	zassert_is_null(open_ch(1u));
	zassert_equal(g_stub.last_error, ALP_ERR_NOMEM);
	zassert_false(g_stub.gd32_stream_active[1], "rollback leaked the GD32 stream");

	alp_adc_stream_close(held);
}

/* MUST stay the last test in this file (and sorted last by name): it
 * permanently leaves slot 0 reserved -- that IS the behaviour under test --
 * and the bookkeeping is process-global, so any later test would see one
 * slot fewer.
 *
 * A supervisor that stays busy through the whole retry budget: the GD32
 * stream is still running, so its slot must NOT be recycled.  The next
 * open takes the other slot instead of hitting the live one. */
ZTEST(adc_stream_close_slot, test_zz_close_keeps_slot_reserved_when_end_undeliverable)
{
	alp_adc_stream_t *h = open_ch(0u);
	zassert_not_null(h);

	g_stub.always_busy = true;
	alp_adc_stream_close(h);
	g_stub.always_busy = false;
	zassert_true(g_stub.gd32_stream_active[0], "stream should still be live");

	alp_adc_stream_t *h2 = open_ch(1u);
	zassert_not_null(h2, "open reused the live slot: last_error=%d", g_stub.last_error);
	zassert_equal(h2->stream_id, 1u, "reopen must skip the still-running slot 0");
	alp_adc_stream_close(h2);
}
