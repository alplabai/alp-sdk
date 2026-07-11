/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Standalone unit tests for <alp/dsp.h> running under native_sim.
 *
 * Covers:
 *   - Chain validation (structural and per-stage bounds).
 *   - Identity FIR (one-tap delta, output == input).
 *   - Direct-form IIR pass-through (b0=1, b1=b2=a1=a2=0).
 *   - FFT bin localisation on a DC signal (energy at bin 0).
 *   - FFT bin localisation on a sinusoid (energy peaked at the
 *     expected bin index for the test signal frequency).
 *   - apply_samples rejecting FFT-terminated chains; apply_bins
 *     rejecting filter-terminated chains.
 *   - One-sided FFT magnitude output (N/2+1 bins) + out-cap check.
 *   - Float-native apply_samples_f32 / apply_bins_f32.
 *   - Q31 coefficients rejected on an IIR stage.
 *   - alp_dsp_stats_f32 summary statistics.
 *   - alp_dsp_biquad_design (RBJ cookbook) coefficients + arg checks.
 *
 * All math kernels run on the portable C fallback (CMSIS-DSP is OFF
 * on native_sim), so the assertions test the fallback paths.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/ztest.h>

#include "alp/adc.h"
#include "alp/dsp.h"
#include "alp/peripheral.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ============================================================== */
/* Validation                                                       */
/* ============================================================== */

ZTEST(alp_dsp_chain_open, test_null_stages_returns_inval)
{
	alp_dsp_chain_t *c = alp_dsp_chain_open(NULL, 1u);
	zassert_is_null(c, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL, NULL);
}

ZTEST(alp_dsp_chain_open, test_zero_stages_returns_inval)
{
	const alp_dsp_stage_t stage = {
		.kind = ALP_DSP_STAGE_FIR,
	};
	alp_dsp_chain_t *c = alp_dsp_chain_open(&stage, 0u);
	zassert_is_null(c, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL, NULL);
}

ZTEST(alp_dsp_chain_open, test_too_many_stages_returns_inval)
{
	alp_dsp_stage_t stages[ALP_DSP_MAX_STAGES + 1u];
	for (size_t i = 0u; i < ALP_DSP_MAX_STAGES + 1u; i++) {
		stages[i].kind               = ALP_DSP_STAGE_FIR;
		stages[i].u.fir.n_taps       = 0u; /* invalid */
		stages[i].u.fir.coeff_format = ALP_DSP_COEFF_FORMAT_F32;
		stages[i].u.fir.taps         = NULL;
	}
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, ALP_DSP_MAX_STAGES + 1u);
	zassert_is_null(c, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL, NULL);
}

ZTEST(alp_dsp_chain_open, test_fft_not_terminal_returns_inval)
{
	alp_dsp_stage_t stages[2] = {
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points = 64u, .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE } },
		{ .kind  = ALP_DSP_STAGE_FIR,
		  .u.fir = { .coeff_format = ALP_DSP_COEFF_FORMAT_F32,
		             .n_taps       = 1u,
		             .taps         = (const float[]){ 1.0f } } },
	};
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 2u);
	zassert_is_null(c, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL, NULL);
}

ZTEST(alp_dsp_chain_open, test_two_fft_stages_returns_inval)
{
	alp_dsp_stage_t stages[2] = {
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points = 64u, .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE } },
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points = 64u, .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE } },
	};
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 2u);
	zassert_is_null(c, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL, NULL);
}

ZTEST(alp_dsp_chain_open, test_window_without_fft_returns_inval)
{
	alp_dsp_stage_t stages[1] = {
		{ .kind = ALP_DSP_STAGE_WINDOW, .u.window = { .shape = ALP_DSP_WINDOW_HANN } },
	};
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 1u);
	zassert_is_null(c, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL, NULL);
}

ZTEST(alp_dsp_chain_open, test_window_not_before_fft_returns_inval)
{
	/* WINDOW must IMMEDIATELY precede FFT.  FIR between them is invalid. */
	alp_dsp_stage_t stages[3] = {
		{ .kind = ALP_DSP_STAGE_WINDOW, .u.window = { .shape = ALP_DSP_WINDOW_HANN } },
		{ .kind  = ALP_DSP_STAGE_FIR,
		  .u.fir = { .coeff_format = ALP_DSP_COEFF_FORMAT_F32,
		             .n_taps       = 1u,
		             .taps         = (const float[]){ 1.0f } } },
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points = 64u, .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE } },
	};
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 3u);
	zassert_is_null(c, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL, NULL);
}

ZTEST(alp_dsp_chain_open, test_fft_n_points_not_power_of_two_returns_range)
{
	alp_dsp_stage_t stages[1] = {
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points      = 100u, /* not a power of two */
		             .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE } },
	};
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 1u);
	zassert_is_null(c, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_OUT_OF_RANGE, NULL);
}

ZTEST(alp_dsp_chain_open, test_fft_n_points_below_min_returns_range)
{
	alp_dsp_stage_t stages[1] = {
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points      = 16u, /* below ALP_DSP_MIN_FFT_POINTS */
		             .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE } },
	};
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 1u);
	zassert_is_null(c, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_OUT_OF_RANGE, NULL);
}

ZTEST(alp_dsp_chain_open, test_fir_zero_taps_returns_range)
{
	alp_dsp_stage_t stages[1] = {
		{ .kind  = ALP_DSP_STAGE_FIR,
		  .u.fir = { .coeff_format = ALP_DSP_COEFF_FORMAT_F32, .n_taps = 0u, .taps = NULL } },
	};
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 1u);
	zassert_is_null(c, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_OUT_OF_RANGE, NULL);
}

ZTEST(alp_dsp_chain_open, test_iir_zero_sections_returns_range)
{
	alp_dsp_stage_t stages[1] = {
		{ .kind  = ALP_DSP_STAGE_IIR,
		  .u.iir = { .coeff_format = ALP_DSP_COEFF_FORMAT_F32, .n_sections = 0u, .coeffs = NULL } },
	};
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 1u);
	zassert_is_null(c, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_OUT_OF_RANGE, NULL);
}

ZTEST(alp_dsp_chain_open, test_valid_single_fir_succeeds)
{
	static const float taps[1] = { 1.0f };
	alp_dsp_stage_t    stage   = { 0 };
	stage.kind                 = ALP_DSP_STAGE_FIR;
	stage.u.fir.coeff_format   = ALP_DSP_COEFF_FORMAT_F32;
	stage.u.fir.n_taps         = 1u;
	stage.u.fir.taps           = taps;
	alp_dsp_chain_t *c         = alp_dsp_chain_open(&stage, 1u);
	zassert_not_null(c, NULL);
	alp_dsp_chain_close(c);
}

ZTEST(alp_dsp_chain_open, test_valid_window_then_fft_succeeds)
{
	alp_dsp_stage_t stages[2] = {
		{ .kind = ALP_DSP_STAGE_WINDOW, .u.window = { .shape = ALP_DSP_WINDOW_HANN } },
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points = 64u, .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE } },
	};
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 2u);
	zassert_not_null(c, NULL);
	alp_dsp_chain_close(c);
}

/* ============================================================== */
/* apply_samples (filter-terminated chains)                         */
/* ============================================================== */

ZTEST(alp_dsp_chain_apply_samples, test_rejects_fft_terminated_chain)
{
	alp_dsp_stage_t stages[1] = {
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points = 64u, .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE } },
	};
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 1u);
	zassert_not_null(c, NULL);

	int16_t      in[64]  = { 0 };
	int16_t      out[64] = { 0 };
	size_t       got     = 0u;
	alp_status_t s       = alp_dsp_chain_apply_samples(c, in, 64u, out, 64u, &got);
	zassert_equal(s, ALP_ERR_NOSUPPORT, NULL);

	alp_dsp_chain_close(c);
}

ZTEST(alp_dsp_chain_apply_samples, test_identity_fir_passes_input_through)
{
	static const float taps[1] = { 1.0f };
	alp_dsp_stage_t    stage   = { 0 };
	stage.kind                 = ALP_DSP_STAGE_FIR;
	stage.u.fir.coeff_format   = ALP_DSP_COEFF_FORMAT_F32;
	stage.u.fir.n_taps         = 1u;
	stage.u.fir.taps           = taps;
	alp_dsp_chain_t *c         = alp_dsp_chain_open(&stage, 1u);
	zassert_not_null(c, NULL);

	static const int16_t in[8]  = { 100, -200, 300, -400, 500, -600, 700, -800 };
	int16_t              out[8] = { 0 };
	size_t               got    = 0u;
	alp_status_t         s      = alp_dsp_chain_apply_samples(c, in, 8u, out, 8u, &got);
	zassert_equal(s, ALP_OK, NULL);
	zassert_equal(got, 8u, NULL);
	for (size_t i = 0u; i < 8u; i++) {
		zassert_equal(out[i], in[i], "identity FIR mismatch at %zu", i);
	}

	alp_dsp_chain_close(c);
}

ZTEST(alp_dsp_chain_apply_samples, test_iir_passthrough_section)
{
	/* Single biquad section configured as identity: y[n] = x[n]. */
	static const float coeffs[5] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f };
	alp_dsp_stage_t    stage     = { 0 };
	stage.kind                   = ALP_DSP_STAGE_IIR;
	stage.u.iir.coeff_format     = ALP_DSP_COEFF_FORMAT_F32;
	stage.u.iir.n_sections       = 1u;
	stage.u.iir.coeffs           = coeffs;
	alp_dsp_chain_t *c           = alp_dsp_chain_open(&stage, 1u);
	zassert_not_null(c, NULL);

	static const int16_t in[4]  = { 1234, -5678, 1111, -2222 };
	int16_t              out[4] = { 0 };
	size_t               got    = 0u;
	alp_status_t         s      = alp_dsp_chain_apply_samples(c, in, 4u, out, 4u, &got);
	zassert_equal(s, ALP_OK, NULL);
	zassert_equal(got, 4u, NULL);
	for (size_t i = 0u; i < 4u; i++) {
		zassert_equal(out[i], in[i], "IIR identity mismatch at %zu", i);
	}

	alp_dsp_chain_close(c);
}

/* ============================================================== */
/* apply_bins (FFT-terminated chains)                              */
/* ============================================================== */

ZTEST(alp_dsp_chain_apply_bins, test_rejects_non_fft_chain)
{
	static const float taps[1] = { 1.0f };
	alp_dsp_stage_t    stage   = { 0 };
	stage.kind                 = ALP_DSP_STAGE_FIR;
	stage.u.fir.coeff_format   = ALP_DSP_COEFF_FORMAT_F32;
	stage.u.fir.n_taps         = 1u;
	stage.u.fir.taps           = taps;
	alp_dsp_chain_t *c         = alp_dsp_chain_open(&stage, 1u);
	zassert_not_null(c, NULL);

	int16_t      in[64]   = { 0 };
	float        out[128] = { 0 };
	size_t       got      = 0u;
	alp_status_t s        = alp_dsp_chain_apply_bins(c, in, 64u, out, 128u, &got);
	zassert_equal(s, ALP_ERR_NOSUPPORT, NULL);

	alp_dsp_chain_close(c);
}

ZTEST(alp_dsp_chain_apply_bins, test_dc_signal_peaks_at_bin_zero)
{
	/* Rectangular window keeps the DC test simple. */
	alp_dsp_stage_t stages[1] = {
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points = 64u, .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE } },
	};
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 1u);
	zassert_not_null(c, NULL);

	int16_t in[64];
	for (size_t i = 0u; i < 64u; i++) {
		in[i] = 1000;
	}
	float        out[64] = { 0 };
	size_t       got     = 0u;
	alp_status_t s       = alp_dsp_chain_apply_bins(c, in, 64u, out, 64u, &got);
	zassert_equal(s, ALP_OK, NULL);
	zassert_equal(got, 64u, NULL);

	/* DC bin should be 64 * 1000 = 64000 (real-only, +/- floating
     * point); other bins should be ~0. */
	zassert_true(out[0] > 60000.0f, "DC bin too low: %f", (double)out[0]);
	for (size_t i = 1u; i < 64u; i++) {
		zassert_true(out[i] < 1.0f, "bin %zu not near zero: %f", i, (double)out[i]);
	}

	alp_dsp_chain_close(c);
}

ZTEST(alp_dsp_chain_apply_bins, test_sine_signal_peaks_at_expected_bin)
{
	/* 64-point FFT, sine at bin 5 (k=5). */
	const uint16_t N = 64u;
	const uint16_t k = 5u;
	int16_t        in[64];
	for (size_t i = 0u; i < N; i++) {
		const double theta = 2.0 * M_PI * (double)k * (double)i / (double)N;
		in[i]              = (int16_t)lrint(2000.0 * sin(theta));
	}

	alp_dsp_stage_t stages[1] = {
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points = 64u, .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE } },
	};
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 1u);
	zassert_not_null(c, NULL);

	float        out[64] = { 0 };
	size_t       got     = 0u;
	alp_status_t s       = alp_dsp_chain_apply_bins(c, in, N, out, N, &got);
	zassert_equal(s, ALP_OK, NULL);
	zassert_equal(got, (size_t)N, NULL);

	/* Magnitude should peak at bin k and its mirror N - k.  The
     * neighbour bins (k+/-1) should be much smaller. */
	zassert_true(out[k] > out[k - 1u], "bin %u not > %u", k, k - 1u);
	zassert_true(out[k] > out[k + 1u], "bin %u not > %u", k, k + 1u);
	zassert_true(out[N - k] > out[N - k - 1u], "mirror bin %u not > %u", N - k, N - k - 1u);
}

ZTEST(alp_dsp_chain_apply_bins, test_complex_output_yields_2n_elements)
{
	alp_dsp_stage_t stages[1] = {
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points = 32u, .output_format = ALP_DSP_FFT_OUTPUT_COMPLEX } },
	};
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 1u);
	zassert_not_null(c, NULL);

	int16_t in[32]       = { 0 };
	in[0]                = 1000;  /* impulse at sample 0 */
	float        out[64] = { 0 }; /* 2 * n_points for COMPLEX */
	size_t       got     = 0u;
	alp_status_t s       = alp_dsp_chain_apply_bins(c, in, 32u, out, 64u, &got);
	zassert_equal(s, ALP_OK, NULL);
	zassert_equal(got, 64u, NULL);

	/* Impulse at n=0 -> every bin has magnitude == in[0]; the real
     * components should equal in[0] (1000) and imaginary parts ~0. */
	for (size_t k_idx = 0u; k_idx < 32u; k_idx++) {
		const float re  = out[2u * k_idx];
		const float im  = out[2u * k_idx + 1u];
		const float mag = sqrtf(re * re + im * im);
		zassert_true(
		    mag > 999.0f && mag < 1001.0f, "bin %zu magnitude off: %f", k_idx, (double)mag);
	}

	alp_dsp_chain_close(c);
}

/* ============================================================== */
/* alp_adc_filter_t (wave-2 streaming ADC + DSP composition)        */
/*                                                                  */
/* Under native_sim there is no V2N supervisor bridge, so the       */
/* filter implementation degrades to ALP_ERR_NOSUPPORT for valid    */
/* args (and surfaces ALP_ERR_INVAL for bad ones via the early-     */
/* validation pre-check).  These tests assert the documented API    */
/* surface contract; HW-in-loop tests cover the real bridge path.   */
/* ============================================================== */

ZTEST(alp_adc_filter, test_open_null_cfg_returns_inval)
{
	alp_adc_filter_t *f = alp_adc_filter_open(NULL);
	zassert_is_null(f, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL, NULL);
}

ZTEST(alp_adc_filter, test_open_null_stages_returns_inval)
{
	alp_adc_filter_config_t cfg = { 0 };
	cfg.channel_id              = 0u;
	cfg.sample_rate_hz          = 1000u;
	cfg.stages                  = NULL;
	cfg.n_stages                = 2u; /* non-zero but stages NULL */
	alp_adc_filter_t *f         = alp_adc_filter_open(&cfg);
	zassert_is_null(f, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL, NULL);
}

ZTEST(alp_adc_filter, test_open_zero_stages_returns_inval)
{
	static const float taps[1] = { 1.0f };
	alp_dsp_stage_t    stage   = { 0 };
	stage.kind                 = ALP_DSP_STAGE_FIR;
	stage.u.fir.coeff_format   = ALP_DSP_COEFF_FORMAT_F32;
	stage.u.fir.n_taps         = 1u;
	stage.u.fir.taps           = taps;

	alp_adc_filter_config_t cfg = { 0 };
	cfg.channel_id              = 0u;
	cfg.sample_rate_hz          = 1000u;
	cfg.stages                  = &stage;
	cfg.n_stages                = 0u;
	alp_adc_filter_t *f         = alp_adc_filter_open(&cfg);
	zassert_is_null(f, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL, NULL);
}

ZTEST(alp_adc_filter, test_open_no_bridge_returns_nosupport)
{
	/* Valid args; native_sim has no V2N supervisor wired so the
     * filter open returns NOSUPPORT after the arg validation passes. */
	static const float taps[1] = { 1.0f };
	alp_dsp_stage_t    stage   = { 0 };
	stage.kind                 = ALP_DSP_STAGE_FIR;
	stage.u.fir.coeff_format   = ALP_DSP_COEFF_FORMAT_F32;
	stage.u.fir.n_taps         = 1u;
	stage.u.fir.taps           = taps;

	alp_adc_filter_config_t cfg = { 0 };
	cfg.channel_id              = 0u;
	cfg.sample_rate_hz          = 1000u;
	cfg.stages                  = &stage;
	cfg.n_stages                = 1u;
	alp_adc_filter_t *f         = alp_adc_filter_open(&cfg);
	zassert_is_null(f, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT, NULL);
}

ZTEST(alp_adc_filter, test_close_null_is_noop)
{
	alp_adc_filter_close(NULL); /* must not crash */
}

ZTEST(alp_adc_filter, test_read_null_handle_returns_not_ready)
{
	int16_t      out[8] = { 0 };
	size_t       got    = 0u;
	alp_status_t s      = alp_adc_filter_read_mv(NULL, out, 8u, &got);
	zassert_equal(s, ALP_ERR_NOT_READY, NULL);
	zassert_equal(got, 0u, NULL);
}

ZTEST(alp_adc_filter, test_read_null_got_returns_inval)
{
	int16_t out[8] = { 0 };
	/* Pass non-NULL handle so we reach the got-NULL check; on
     * native_sim with no bridge we can't actually open a filter, so
     * synthesize a non-NULL but unrelated pointer.  The function's
     * first check is `got == NULL`, which is independent of handle
     * validity. */
	int          dummy = 0;
	alp_status_t s     = alp_adc_filter_read_mv((alp_adc_filter_t *)&dummy, out, 8u, NULL);
	zassert_equal(s, ALP_ERR_INVAL, NULL);
}

/* ============================================================== */
/* alp_adc_spectrum_t (wave-2 §2B.1(c) FFT-terminated chain)        */
/*                                                                  */
/* On native_sim without a V2N supervisor the spectrum_open call    */
/* returns NOSUPPORT for valid args -- same NOSUPPORT contract as   */
/* the filter sibling.  Wrong-entry-point detection (filter chain   */
/* passed to spectrum_open) still surfaces INVAL.                  */
/* ============================================================== */

ZTEST(alp_adc_spectrum, test_open_null_cfg_returns_inval)
{
	alp_adc_spectrum_t *spec = alp_adc_spectrum_open(NULL);
	zassert_is_null(spec, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL, NULL);
}

ZTEST(alp_adc_spectrum, test_open_null_stages_returns_inval)
{
	const alp_adc_spectrum_config_t cfg = {
		.channel_id     = 0u,
		.sample_rate_hz = 1000u,
		.stages         = NULL,
		.n_stages       = 1u,
	};
	alp_adc_spectrum_t *spec = alp_adc_spectrum_open(&cfg);
	zassert_is_null(spec, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL, NULL);
}

ZTEST(alp_adc_spectrum, test_open_zero_stages_returns_inval)
{
	alp_dsp_stage_t stage     = { 0 };
	stage.kind                = ALP_DSP_STAGE_FFT;
	stage.u.fft.n_points      = 64u;
	stage.u.fft.output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE;

	const alp_adc_spectrum_config_t cfg = {
		.channel_id     = 0u,
		.sample_rate_hz = 1000u,
		.stages         = &stage,
		.n_stages       = 0u,
	};
	alp_adc_spectrum_t *spec = alp_adc_spectrum_open(&cfg);
	zassert_is_null(spec, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL, NULL);
}

ZTEST(alp_adc_spectrum, test_open_filter_terminated_chain_returns_inval)
{
	/* Wrong entry point: filter-terminated chain passed to spectrum_open. */
	static const float taps[1] = { 1.0f };
	alp_dsp_stage_t    stage   = { 0 };
	stage.kind                 = ALP_DSP_STAGE_FIR;
	stage.u.fir.coeff_format   = ALP_DSP_COEFF_FORMAT_F32;
	stage.u.fir.n_taps         = 1u;
	stage.u.fir.taps           = taps;

	const alp_adc_spectrum_config_t cfg = {
		.channel_id     = 0u,
		.sample_rate_hz = 1000u,
		.stages         = &stage,
		.n_stages       = 1u,
	};
	alp_adc_spectrum_t *spec = alp_adc_spectrum_open(&cfg);
	zassert_is_null(spec, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_INVAL, NULL);
}

ZTEST(alp_adc_spectrum, test_open_fft_chain_no_bridge_returns_nosupport)
{
	alp_dsp_stage_t stage     = { 0 };
	stage.kind                = ALP_DSP_STAGE_FFT;
	stage.u.fft.n_points      = 64u;
	stage.u.fft.output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE;

	const alp_adc_spectrum_config_t cfg = {
		.channel_id     = 0u,
		.sample_rate_hz = 1000u,
		.stages         = &stage,
		.n_stages       = 1u,
	};
	alp_adc_spectrum_t *spec = alp_adc_spectrum_open(&cfg);
	zassert_is_null(spec, NULL);
	zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT, NULL);
}

ZTEST(alp_adc_spectrum, test_close_null_is_noop)
{
	alp_adc_spectrum_close(NULL); /* must not crash */
}

ZTEST(alp_adc_spectrum, test_read_null_handle_returns_not_ready)
{
	float        bins[128] = { 0 };
	size_t       got       = 0u;
	alp_status_t s         = alp_adc_spectrum_read_bins(NULL, bins, 128u, &got);
	zassert_equal(s, ALP_ERR_NOT_READY, NULL);
	zassert_equal(got, 0u, NULL);
}

ZTEST(alp_adc_spectrum, test_read_null_got_returns_inval)
{
	float        bins[128] = { 0 };
	int          dummy     = 0;
	alp_status_t s = alp_adc_spectrum_read_bins((alp_adc_spectrum_t *)&dummy, bins, 128u, NULL);
	zassert_equal(s, ALP_ERR_INVAL, NULL);
}

/* ---- alp_dsp_stats_f32 (portable scalar-stats surface) ------------ */

ZTEST(alp_dsp_stats, test_null_args_return_inval)
{
	float           x[3] = { 1.0f, 2.0f, 3.0f };
	alp_dsp_stats_t s;
	zassert_equal(alp_dsp_stats_f32(NULL, 3u, &s), ALP_ERR_INVAL, NULL);
	zassert_equal(alp_dsp_stats_f32(x, 3u, NULL), ALP_ERR_INVAL, NULL);
	zassert_equal(alp_dsp_stats_f32(x, 0u, &s), ALP_ERR_INVAL, NULL);
}

ZTEST(alp_dsp_stats, test_ramp_stats)
{
	/* x = 1..5: mean 3, rms sqrt(11), var 2, peak |5| at index 4. */
	float           x[5] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f };
	alp_dsp_stats_t s;
	zassert_equal(alp_dsp_stats_f32(x, 5u, &s), ALP_OK, NULL);
	zassert_within(s.mean, 3.0f, 1e-4f, NULL);
	zassert_within(s.rms, sqrtf(11.0f), 1e-4f, NULL);
	zassert_within(s.variance, 2.0f, 1e-3f, NULL);
	zassert_within(s.min, 1.0f, 1e-4f, NULL);
	zassert_within(s.max, 5.0f, 1e-4f, NULL);
	zassert_within(s.abs_max, 5.0f, 1e-4f, NULL);
	zassert_equal(s.abs_max_index, 4u, NULL);
}

ZTEST(alp_dsp_stats, test_signed_peak_and_variance)
{
	/* x = {-4,1,-6,2}: mean -1.75, peak |−6| at index 2, min −6, max 2. */
	float           x[4] = { -4.0f, 1.0f, -6.0f, 2.0f };
	alp_dsp_stats_t s;
	zassert_equal(alp_dsp_stats_f32(x, 4u, &s), ALP_OK, NULL);
	zassert_within(s.mean, -1.75f, 1e-4f, NULL);
	zassert_within(s.min, -6.0f, 1e-4f, NULL);
	zassert_within(s.max, 2.0f, 1e-4f, NULL);
	zassert_within(s.abs_max, 6.0f, 1e-4f, NULL);
	zassert_equal(s.abs_max_index, 2u, NULL);
	zassert_within(s.variance, 11.1875f, 1e-3f, NULL);
	zassert_true(s.variance >= 0.0f, NULL);
}

ZTEST(alp_dsp_stats, test_constant_signal_zero_variance)
{
	/* A constant signal: variance must clamp to exactly >= 0 (no FP -eps). */
	float           x[6] = { 2.5f, 2.5f, 2.5f, 2.5f, 2.5f, 2.5f };
	alp_dsp_stats_t s;
	zassert_equal(alp_dsp_stats_f32(x, 6u, &s), ALP_OK, NULL);
	zassert_within(s.mean, 2.5f, 1e-4f, NULL);
	zassert_within(s.rms, 2.5f, 1e-4f, NULL);
	zassert_true(s.variance >= 0.0f, NULL);
	zassert_within(s.variance, 0.0f, 1e-3f, NULL);
}

/* ---- one-sided FFT magnitude (N/2+1 bins) ------------------------- */

ZTEST(alp_dsp_chain_apply_bins, test_onesided_dc_bin_count_and_peak)
{
	const uint16_t  N         = 64u;
	const size_t    half1     = N / 2u + 1u;
	alp_dsp_stage_t stages[1] = {
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points = N, .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE_ONESIDED } },
	};
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 1u);
	zassert_not_null(c, NULL);

	int16_t in[64];
	for (size_t i = 0u; i < N; i++) {
		in[i] = 1000;
	}
	float        out[64] = { 0 };
	size_t       got     = 0u;
	alp_status_t s       = alp_dsp_chain_apply_bins(c, in, N, out, half1, &got);
	zassert_equal(s, ALP_OK, NULL);
	zassert_equal(got, half1, "one-sided count wrong: %zu", got); /* 33, not 64 */
	zassert_true(out[0] > 60000.0f, "DC bin too low: %f", (double)out[0]);
	for (size_t i = 1u; i < half1; i++) {
		zassert_true(out[i] < 1.0f, "bin %zu not near zero: %f", i, (double)out[i]);
	}
	alp_dsp_chain_close(c);
}

ZTEST(alp_dsp_chain_apply_bins, test_onesided_out_cap_too_small_rejected)
{
	alp_dsp_stage_t stages[1] = {
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points = 64u, .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE_ONESIDED } },
	};
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 1u);
	zassert_not_null(c, NULL);
	int16_t in[64]  = { 0 };
	float   out[64] = { 0 };
	size_t  got     = 0u;
	/* Needs 33 bins; offering 32 must be rejected. */
	zassert_equal(alp_dsp_chain_apply_bins(c, in, 64u, out, 32u, &got), ALP_ERR_OUT_OF_RANGE, NULL);
	alp_dsp_chain_close(c);
}

/* ---- float-native apply variants ---------------------------------- */

ZTEST(alp_dsp_chain_apply_samples, test_f32_unity_fir_passthrough)
{
	const float     tap       = 1.0f; /* single unity tap = identity */
	alp_dsp_stage_t stages[1] = {
		{ .kind  = ALP_DSP_STAGE_FIR,
		  .u.fir = { .coeff_format = ALP_DSP_COEFF_FORMAT_F32, .n_taps = 1u, .taps = &tap } },
	};
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 1u);
	zassert_not_null(c, NULL);
	float        in[8]  = { 0.5f, -1.5f, 2.25f, 0.0f, 3.0f, -0.75f, 1.0f, -2.0f };
	float        out[8] = { 0 };
	size_t       got    = 0u;
	alp_status_t s      = alp_dsp_chain_apply_samples_f32(c, in, 8u, out, 8u, &got);
	zassert_equal(s, ALP_OK, NULL);
	zassert_equal(got, 8u, NULL);
	for (size_t i = 0u; i < 8u; i++) {
		zassert_within(out[i], in[i], 1e-5f, "f32 sample %zu", i);
	}
	alp_dsp_chain_close(c);
}

ZTEST(alp_dsp_chain_apply_bins, test_f32_dc_peaks_at_bin_zero)
{
	alp_dsp_stage_t stages[1] = {
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points = 32u, .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE_ONESIDED } },
	};
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 1u);
	zassert_not_null(c, NULL);
	float in[32];
	for (size_t i = 0u; i < 32u; i++) {
		in[i] = 2.0f;
	}
	float        out[17] = { 0 };
	size_t       got     = 0u;
	alp_status_t s       = alp_dsp_chain_apply_bins_f32(c, in, 32u, out, 17u, &got);
	zassert_equal(s, ALP_OK, NULL);
	zassert_equal(got, 17u, NULL);
	zassert_true(out[0] > 60.0f, "DC bin %f", (double)out[0]); /* 32*2 = 64 */
	for (size_t i = 1u; i < 17u; i++) {
		zassert_true(out[i] < 1e-3f, "bin %zu %f", i, (double)out[i]);
	}
	alp_dsp_chain_close(c);
}

/* ---- Q31 coefficients rejected on IIR ----------------------------- */

ZTEST(alp_dsp_chain_open, test_q31_iir_rejected)
{
	const int32_t   coeffs[5] = { 0, 0, 0, 0, 0 };
	alp_dsp_stage_t stages[1] = {
		{ .kind  = ALP_DSP_STAGE_IIR,
		  .u.iir = { .coeff_format = ALP_DSP_COEFF_FORMAT_Q31,
		             .n_sections   = 1u,
		             .coeffs       = coeffs } },
	};
	/* Q31 IIR would silently wrap a1 (|a1| up to 2 > Q31's +/-1) -- the
	 * backend rejects it, so open must fail. */
	alp_dsp_chain_t *c = alp_dsp_chain_open(stages, 1u);
	zassert_is_null(c, NULL);
}

/* ---- biquad designer (alp_dsp_biquad_design) ---------------------- */

ZTEST(alp_dsp_biquad, test_butterworth_lowpass_coeffs)
{
	/* LP fc=50 fs=1000 Q=1/sqrt(2): known RBJ coefficients. */
	float        c[5];
	alp_status_t s = alp_dsp_biquad_design(ALP_DSP_BIQUAD_LOWPASS, 50.0f, 1000.0f, 0.70710678f, c);
	zassert_equal(s, ALP_OK, NULL);
	zassert_within(c[0], 0.0201f, 5e-3f, "b0 %f", (double)c[0]);
	zassert_within(c[1], 0.0402f, 5e-3f, "b1 %f", (double)c[1]);
	zassert_within(c[2], 0.0201f, 5e-3f, "b2 %f", (double)c[2]);
	zassert_within(c[3], -1.5610f, 5e-3f, "a1 %f", (double)c[3]);
	zassert_within(c[4], 0.6414f, 5e-3f, "a2 %f", (double)c[4]);
}

ZTEST(alp_dsp_biquad, test_all_kinds_succeed)
{
	float c[5];
	zassert_equal(
	    alp_dsp_biquad_design(ALP_DSP_BIQUAD_LOWPASS, 100.0f, 8000.0f, 0.707f, c), ALP_OK, NULL);
	zassert_equal(
	    alp_dsp_biquad_design(ALP_DSP_BIQUAD_HIGHPASS, 100.0f, 8000.0f, 0.707f, c), ALP_OK, NULL);
	zassert_equal(
	    alp_dsp_biquad_design(ALP_DSP_BIQUAD_BANDPASS, 100.0f, 8000.0f, 2.0f, c), ALP_OK, NULL);
	zassert_equal(
	    alp_dsp_biquad_design(ALP_DSP_BIQUAD_NOTCH, 60.0f, 8000.0f, 5.0f, c), ALP_OK, NULL);
}

ZTEST(alp_dsp_biquad, test_invalid_args_rejected)
{
	float c[5];
	zassert_equal(alp_dsp_biquad_design(ALP_DSP_BIQUAD_LOWPASS, 50.0f, 1000.0f, 0.7f, NULL),
	              ALP_ERR_INVAL,
	              NULL);
	/* f0 at/above Nyquist. */
	zassert_equal(alp_dsp_biquad_design(ALP_DSP_BIQUAD_LOWPASS, 500.0f, 1000.0f, 0.7f, c),
	              ALP_ERR_INVAL,
	              NULL);
	/* q <= 0, fs <= 0, f0 <= 0. */
	zassert_equal(alp_dsp_biquad_design(ALP_DSP_BIQUAD_LOWPASS, 50.0f, 1000.0f, 0.0f, c),
	              ALP_ERR_INVAL,
	              NULL);
	zassert_equal(
	    alp_dsp_biquad_design(ALP_DSP_BIQUAD_LOWPASS, 50.0f, 0.0f, 0.7f, c), ALP_ERR_INVAL, NULL);
	zassert_equal(
	    alp_dsp_biquad_design(ALP_DSP_BIQUAD_LOWPASS, 0.0f, 1000.0f, 0.7f, c), ALP_ERR_INVAL, NULL);
}

/* Test suites are auto-collected via ztest_register macros below.   */
ZTEST_SUITE(alp_dsp_chain_open, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(alp_dsp_chain_apply_samples, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(alp_dsp_chain_apply_bins, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(alp_dsp_stats, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(alp_dsp_biquad, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(alp_adc_filter, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(alp_adc_spectrum, NULL, NULL, NULL, NULL, NULL);
