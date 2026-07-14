/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * butterworth-lowpass -- design a 2nd-order Butterworth low-pass filter
 * and run it through the portable <alp/dsp.h> IIR chain stage.
 * =====================================================================
 *
 * WHAT THIS DEMONSTRATES
 * ----------------------
 * The companion FFT examples (rail-predictive-maintenance, ...) show the
 * spectral side of <alp/dsp>.  This one shows the FILTER side: the same
 * chain API carries a cascaded-biquad IIR stage (ALP_DSP_STAGE_IIR).  The
 * backend runs CMSIS-DSP `arm_biquad_cascade_df1_f32` on the Cortex-M55
 * and a portable-C biquad under native_sim -- the app never touches
 * `arm_*`; it just designs the coefficients and hands them to the chain.
 *
 * The demo:
 *   1. Designs a 2nd-order Butterworth low-pass biquad with the SDK's
 *      alp_dsp_biquad_design() (cookbook low-pass at Q = 1/sqrt(2)) --
 *      no hand-derived filter math in the app.
 *   2. Pushes TWO float test tones through the same filter with
 *      alp_dsp_chain_apply_samples_f32 (no int16 round-trip): one in the
 *      passband (well below FC) and one in the stopband (well above FC).
 *   3. Measures each tone's RMS before and after with alp_dsp_stats_f32
 *      and reports the gain -- the passband tone passes (~unity), the
 *      stopband tone is strongly attenuated.
 *
 * TARGET / RUN
 * ------------
 * SoM-agnostic pure compute -- no peripherals.  On native_sim the whole
 * pipeline runs to completion against synthetic tones:
 *   west build -b native_sim/native/64 examples/audio/butterworth-lowpass
 *   west build -t run
 * On the E1M-AEN801 (M55) the identical source runs the CMSIS-DSP biquad.
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include <alp/dsp.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * Signal parameters.  FS is the sample rate; FC the filter's -3 dB
 * cutoff.  N samples give the filter time to settle and leave a long
 * steady-state stretch to measure.  Amplitude AMP is unit-scale -- the
 * float chain has no fixed-point range to fill.
 */
#define FS_HZ    1000.0f
#define FC_HZ    50.0f
#define N_SAMPLE 1024u
#define SETTLE   128u /* skip the biquad's start-up transient before RMS */
#define AMP      1.0f /* unit amplitude; the float chain has no int16 range */

/* Two probe tones: one an octave-plus below FC (passband), one well
 * above (stopband).  250 Hz is 5x FC -> ~2.3 octaves -> a 2nd-order
 * (-12 dB/octave) roll-off predicts roughly -28 dB (~0.04x) there. */
#define TONE_PASS_HZ 10.0f
#define TONE_STOP_HZ 250.0f

/* Fill buf with a unit-amplitude float sine of frequency freq_hz. */
static void synth_tone(float freq_hz, float *buf, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		float t = (float)i / FS_HZ;
		buf[i]  = AMP * sinf(2.0f * (float)M_PI * freq_hz * t);
	}
}

/*
 * rms_steady -- RMS of the buffer over its steady-state region.
 *
 * The first SETTLE samples are dropped: a freshly-opened biquad starts
 * from zero state, so its output ramps up over the first few time
 * constants.  Measuring only [SETTLE, n) reports the filter's true
 * steady-state gain.  One alp_dsp_stats_f32 call does the reduction --
 * CMSIS arm_rms_f32 on the M55, portable-C otherwise.
 */
static float rms_steady(const float *buf, size_t n)
{
	alp_dsp_stats_t s;
	if (alp_dsp_stats_f32(buf + SETTLE, n - SETTLE, &s) != ALP_OK) {
		return 0.0f;
	}
	return s.rms;
}

/*
 * run_tone -- filter one probe tone and report its passband/stopband gain.
 *
 * Opens a one-section IIR chain from the shared biquad coefficients and
 * pushes the float tone through it with alp_dsp_chain_apply_samples_f32
 * (float in -> float out, no int16 round-trip).  Returns the output/input
 * steady-state RMS ratio -- the filter's gain at that frequency: ~1.0
 * means "passed", << 1.0 means "rejected".
 */
static float run_tone(const float coeffs[5], float freq_hz)
{
	static float in[N_SAMPLE];
	static float out[N_SAMPLE];
	synth_tone(freq_hz, in, N_SAMPLE);

	alp_dsp_stage_t stages[] = {
		{ .kind  = ALP_DSP_STAGE_IIR,
		  .u.iir = { .coeff_format = ALP_DSP_COEFF_FORMAT_F32,
		             .n_sections   = 1u,
		             .coeffs       = coeffs } },
	};
	alp_dsp_chain_t *chain = alp_dsp_chain_open(stages, 1u);
	if (chain == NULL) {
		printf("[bwlp] chain open failed (is CONFIG_ALP_SDK_DSP set?)\n");
		return -1.0f;
	}

	size_t       got = 0;
	alp_status_t st  = alp_dsp_chain_apply_samples_f32(chain, in, N_SAMPLE, out, N_SAMPLE, &got);
	alp_dsp_chain_close(chain);
	if (st != ALP_OK || got < N_SAMPLE) {
		printf("[bwlp] filter apply failed (st=%d got=%zu)\n", (int)st, got);
		return -1.0f;
	}

	float in_rms  = rms_steady(in, N_SAMPLE);
	float out_rms = rms_steady(out, N_SAMPLE);
	return (in_rms > 1e-6f) ? (out_rms / in_rms) : 0.0f;
}

int main(void)
{
	printf("[bwlp] 2nd-order Butterworth low-pass via <alp/dsp> IIR chain\n");
	printf("[bwlp] fs=%.0f Hz  fc=%.0f Hz\n", (double)FS_HZ, (double)FC_HZ);

	/* Design once with the SDK's biquad designer -- a 2nd-order
	 * Butterworth low-pass is a cookbook low-pass at Q = 1/sqrt(2).  No
	 * hand-derived filter math in the app; both probe tones share it. */
	float        coeffs[5];
	alp_status_t ds =
	    alp_dsp_biquad_design(ALP_DSP_BIQUAD_LOWPASS, FC_HZ, FS_HZ, 0.70710678f, coeffs);
	if (ds != ALP_OK) {
		printf("[bwlp] biquad design failed (st=%d)\n", (int)ds);
		return 0;
	}
	printf("[bwlp] biquad {b0,b1,b2,a1,a2} = {%.4f, %.4f, %.4f, %.4f, %.4f}\n",
	       (double)coeffs[0],
	       (double)coeffs[1],
	       (double)coeffs[2],
	       (double)coeffs[3],
	       (double)coeffs[4]);

	/* Passband tone should survive; stopband tone should be crushed. */
	float pass_gain = run_tone(coeffs, TONE_PASS_HZ);
	float stop_gain = run_tone(coeffs, TONE_STOP_HZ);
	if (pass_gain < 0.0f || stop_gain < 0.0f) {
		return 0; /* error already logged */
	}
	printf("[bwlp] %6.1f Hz (passband): gain %.3f\n", (double)TONE_PASS_HZ, (double)pass_gain);
	printf("[bwlp] %6.1f Hz (stopband): gain %.3f (%.1f dB)\n",
	       (double)TONE_STOP_HZ,
	       (double)stop_gain,
	       (double)(20.0f * log10f(stop_gain > 1e-6f ? stop_gain : 1e-6f)));

	/*
	 * Acceptance: the passband tone keeps most of its amplitude, the
	 * stopband tone loses most of its.  Generous margins so the check is
	 * about the filter WORKING, not about matched-to-the-decimal design.
	 */
	bool ok = (pass_gain > 0.7f) && (stop_gain < 0.25f);
	printf("[bwlp] %s\n",
	       ok ? "PASS: passband preserved, stopband rejected" : "FAIL: unexpected gains");

	printf("[bwlp] done\n");
	return 0;
}
