/*
 * Copyright 2026 ALP Lab AB
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
 *
 * All math kernels run on the portable C fallback (CMSIS-DSP is OFF
 * on native_sim), so the assertions test the fallback paths.
 */

#include <math.h>
#include <stdint.h>
#include <stdlib.h>

#include <zephyr/ztest.h>

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
    alp_dsp_stage_t stage = { 0 };
    stage.kind = ALP_DSP_STAGE_FIR;
    stage.u.fir.coeff_format = ALP_DSP_COEFF_FORMAT_F32;
    stage.u.fir.n_taps = 1u;
    stage.u.fir.taps = taps;
    alp_dsp_chain_t *c = alp_dsp_chain_open(&stage, 1u);
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
    alp_dsp_stage_t stage = { 0 };
    stage.kind = ALP_DSP_STAGE_FIR;
    stage.u.fir.coeff_format = ALP_DSP_COEFF_FORMAT_F32;
    stage.u.fir.n_taps = 1u;
    stage.u.fir.taps = taps;
    alp_dsp_chain_t *c = alp_dsp_chain_open(&stage, 1u);
    zassert_not_null(c, NULL);

    static const int16_t in[8] = { 100, -200, 300, -400, 500, -600, 700, -800 };
    int16_t out[8] = { 0 };
    size_t got = 0u;
    alp_status_t s = alp_dsp_chain_apply_samples(c, in, 8u, out, 8u, &got);
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
    alp_dsp_stage_t stage = { 0 };
    stage.kind = ALP_DSP_STAGE_IIR;
    stage.u.iir.coeff_format = ALP_DSP_COEFF_FORMAT_F32;
    stage.u.iir.n_sections = 1u;
    stage.u.iir.coeffs = coeffs;
    alp_dsp_chain_t *c = alp_dsp_chain_open(&stage, 1u);
    zassert_not_null(c, NULL);

    static const int16_t in[4] = { 1234, -5678, 1111, -2222 };
    int16_t out[4] = { 0 };
    size_t got = 0u;
    alp_status_t s = alp_dsp_chain_apply_samples(c, in, 4u, out, 4u, &got);
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
    alp_dsp_stage_t stage = { 0 };
    stage.kind = ALP_DSP_STAGE_FIR;
    stage.u.fir.coeff_format = ALP_DSP_COEFF_FORMAT_F32;
    stage.u.fir.n_taps = 1u;
    stage.u.fir.taps = taps;
    alp_dsp_chain_t *c = alp_dsp_chain_open(&stage, 1u);
    zassert_not_null(c, NULL);

    int16_t in[64] = { 0 };
    float out[128] = { 0 };
    size_t got = 0u;
    alp_status_t s = alp_dsp_chain_apply_bins(c, in, 64u, out, 128u, &got);
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
        zassert_true(mag > 999.0f && mag < 1001.0f, "bin %zu magnitude off: %f", k_idx,
                     (double)mag);
    }

    alp_dsp_chain_close(c);
}

/* Test suites are auto-collected via ztest_register macros below.   */
ZTEST_SUITE(alp_dsp_chain_open, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(alp_dsp_chain_apply_samples, NULL, NULL, NULL, NULL, NULL);
ZTEST_SUITE(alp_dsp_chain_apply_bins, NULL, NULL, NULL, NULL, NULL);
