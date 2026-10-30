/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Two regressions in the BEGIN2 / spectrum paths of peripheral_adc.c:
 *
 *  - a BEGIN2 reply with full_scale 0 is rolled back with STREAM_END; when
 *    that STREAM_END cannot be delivered the GD32 stream is still live, so
 *    the slot must stay reserved (the old code freed it unconditionally).
 *  - alp_adc_spectrum_read_bins() must drop a partial FFT block when the
 *    stream read reports a discontinuity of unknown length (ALP_ERR_BUSY).
 *
 * Harness and stubs are shared with tests/unit/adc_stream_close_slot; see
 * CMakeLists.txt for why this lives in its own binary.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <alp/adc.h>
#include <alp/dsp.h>

#include "handles.h"
#include "stubs.h"

#define FFT_N          64u
#define CODES_PER_READ 4u

static void each_before(void *unused)
{
	(void)unused;
	memset(&g_stub, 0, sizeof(g_stub));
}

ZTEST_SUITE(adc_stream_begin2_rollback, NULL, NULL, each_before, NULL, NULL);

/* A gap mid-block drops the accumulated partial block: after the BUSY the
 * next read_bins needs a FULL block again (FFT_N / CODES_PER_READ reads);
 * without the reset it would need one read fewer.  Runs first by name and
 * closes cleanly, so it leaves no slot behind. */
ZTEST(adc_stream_begin2_rollback, test_a_spectrum_gap_drops_the_partial_block)
{
	g_stub.grant_stream2 = true;
	g_stub.full_scale    = 4095u;
	g_stub.vref_mv       = 1800u;
	g_stub.chain_ok      = true;

	const alp_dsp_stage_t stages[] = {
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points = FFT_N, .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE } },
	};
	alp_adc_spectrum_config_t cfg = ALP_ADC_SPECTRUM_CONFIG_DEFAULT(0u);
	cfg.sample_rate_hz            = 1000u;
	cfg.stages                    = stages;
	cfg.n_stages                  = 1u;
	alp_adc_spectrum_t *spec      = alp_adc_spectrum_open(&cfg);
	zassert_not_null(spec, "last_error=%d", g_stub.last_error);

	static float bins[FFT_N];
	size_t       got = 0u;

	/* Two reads of CODES_PER_READ accumulate a partial block, then the
	 * third read reports the discontinuity. */
	g_stub.r2_got          = CODES_PER_READ;
	g_stub.r2_busy_on_call = 3u;
	zassert_equal(alp_adc_spectrum_read_bins(spec, bins, FFT_N, &got), ALP_ERR_BUSY);
	zassert_equal(g_stub.read2_calls, 3u);

	/* Fresh block: exactly FFT_N / CODES_PER_READ reads, then the (stubbed)
	 * chain refuses.  A surviving partial block would take 2 reads fewer. */
	g_stub.read2_calls     = 0u;
	g_stub.r2_busy_on_call = 0u;
	zassert_equal(alp_adc_spectrum_read_bins(spec, bins, FFT_N, &got), ALP_ERR_NOSUPPORT);
	zassert_equal(g_stub.read2_calls, FFT_N / CODES_PER_READ, "partial block survived the gap");
	alp_adc_spectrum_close(spec);
}

/* MUST stay the last test in this file (sorted last by name): it
 * permanently leaves slot 0 reserved -- that IS the behaviour under test --
 * and the bookkeeping is process-global.
 *
 * BEGIN2 answered with no scaling and the rollback STREAM_END cannot be
 * delivered: the GD32 stream is still live, so the slot must stay reserved
 * (the next open takes the other slot, never the live one). */
ZTEST(adc_stream_begin2_rollback, test_zz_unscaled_begin_keeps_slot_when_end_fails)
{
	g_stub.grant_stream2 = true;
	g_stub.full_scale    = 0u;
	g_stub.end_fails     = true;
	zassert_is_null(alp_adc_stream_open(
	    &(alp_adc_stream_config_t){ .channel_id = 0u, .sample_rate_hz = 1000u }));
	zassert_equal(g_stub.last_error, ALP_ERR_IO);
	zassert_true(g_stub.gd32_stream_active[0], "END failed: GD32 stream still running");

	g_stub.end_fails    = false;
	g_stub.full_scale   = 4095u;
	g_stub.vref_mv      = 1800u;
	alp_adc_stream_t *h = alp_adc_stream_open(
	    &(alp_adc_stream_config_t){ .channel_id = 0u, .sample_rate_hz = 1000u });
	zassert_not_null(h);
	zassert_true(g_stub.gd32_stream_active[1], "slot 0 stays reserved; the open takes slot 1");
	alp_adc_stream_close(h);
}
