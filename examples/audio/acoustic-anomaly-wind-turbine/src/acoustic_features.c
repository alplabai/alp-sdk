/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * acoustic_features implementation -- see acoustic_features.h.
 */
#include "acoustic_features.h"

#include <math.h>
#include <string.h>

/*
 * SPECTRAL + STATISTICAL MATH -- via <alp/dsp.h>, not hand-rolled.
 * ---------------------------------------------------------------
 * This example does NOT re-derive an FFT or the time-domain moment
 * statistics by hand, and it does NOT call CMSIS-DSP arm_* directly.
 * Both go through the portable <alp/dsp.h> surface, so the SAME source
 * builds for native_sim and the Cortex-M55 alike and the SDK -- not the
 * app -- owns the CMSIS-vs-portable choice:
 *
 *   1. FFT -> alp_dsp_chain (ALP_DSP_STAGE_FFT).  The backend runs
 *      CMSIS-DSP arm_rfft_fast_f32 (Helium-vectorised) on the M55, a
 *      hardware DSP block where the SoM has one, and a portable-C radix-2
 *      fallback under native_sim -- selected by the backend registry.
 *
 *   2. Scalar window statistics (mean, RMS, abs-peak) -> alp_dsp_stats_f32,
 *      which the SDK backs with CMSIS-DSP arm_mean/rms/absmax_f32 on
 *      Cortex-M and a portable-C pass elsewhere.  Keeping both on
 *      <alp/dsp> lets the identical source run on the V2N (A55 + DRP-AI)
 *      and NXP paths, which have no CMSIS-M.
 *
 * Only kurtosis stays a hand-written loop -- it is a 4th standardised
 * moment with no CMSIS (or alp_dsp) kernel.
 */
#include <alp/dsp.h>

/* ---------------------------------------------------------------------------
 * Frame accumulator helpers.
 *
 * Samples arrive one at a time from the codec DMA callback.  The ring of
 * ACO_FRAME_N samples is filled linearly (count tracks how many are valid);
 * when count reaches ACO_FRAME_N the caller may extract features and then
 * call aco_frame_reset() to start the next frame.
 * ---------------------------------------------------------------------------
 */
void aco_frame_reset(struct aco_frame_state *st)
{
	st->count = 0;
}

void aco_frame_push(struct aco_frame_state *st, float sample)
{
	if (st->count < ACO_FRAME_N) {
		st->samples[st->count++] = sample;
	}
}

bool aco_frame_full(const struct aco_frame_state *st)
{
	return st->count >= ACO_FRAME_N;
}

/* ---------------------------------------------------------------------------
 * aco_feat_extract -- extract all per-frame features from a filled frame.
 *
 * Three sequential sub-passes over the samples:
 *
 *   Sub-pass A (time domain): DC removal, AC variance, kurtosis.
 *   Sub-pass B (frequency domain): FFT, spectral centroid, spectral flatness.
 *   Sub-pass C (band energy): log-spaced band accumulation, normalisation.
 *
 * All outputs are zeroed first; any early-exit on a silent frame leaves them
 * at zero, which is a safe sentinel the anomaly scorer can detect.
 * ---------------------------------------------------------------------------
 */
void aco_feat_extract(const struct aco_frame_state *st, float sr_hz, struct aco_features *out)
{
	const int n = (st->count < ACO_FRAME_N) ? st->count : ACO_FRAME_N;

	memset(out, 0, sizeof(*out));
	if (n <= 0) {
		return;
	}

	/* ----- Sub-pass A: time-domain statistics --------------------------------
	 *
	 * Step A1 -- DC removal.
	 *   Remove the sample mean before computing any feature.  This eliminates
	 *   microphone offset and slow ambient-pressure drift so that RMS and
	 *   kurtosis reflect only the AC (vibration) content.
	 *
	 * Step A2 -- Variance and RMS.
	 *   var = (1/n) * sum( (x_i - mu)^2 )
	 *   total_rms = sqrt(var)
	 *   RMS is the most direct proxy for vibration amplitude; a healthy
	 *   turbine has a narrow operating band; sustained elevation flags overload.
	 *
	 * Step A3 -- Kurtosis (4th standardised central moment).
	 *   kurtosis = E[(x-mu)^4] / (E[(x-mu)^2])^2  =  (sum4/n) / var^2
	 *   For a Gaussian signal kurtosis ≈ 3.  Impulsive fault signatures such
	 *   as rolling-element bearing spall impacts push kurtosis well above 10.
	 *   Division is guarded by a near-zero variance threshold to avoid NaN.
	 * --------------------------------------------------------------------------
	 */
	/*
	 * xc[] holds the DC-removed window shared by the time-domain stats AND
	 * the FFT below; the tail [n, ACO_FRAME_N) is zero-padded so a short
	 * frame can still feed the fixed-size FFT.  static: aco_feat_extract is
	 * not re-entrant (single codec path), keeps a 1 kB buffer off the stack.
	 *
	 * One alp_dsp_stats_f32 pass over the raw window yields the mean
	 * (Step A1); a second over the mean-centred buffer yields the AC RMS
	 * and abs-peak (Step A2, plus the peak used to scale the FFT feed
	 * below) with the DC bias removed in one shot.  The SDK backs these
	 * with CMSIS-DSP arm_mean/rms/absmax_f32 on the M55 and a portable-C
	 * pass under native_sim -- no arm_* here.
	 */
	static float xc[ACO_FRAME_N];

	alp_dsp_stats_t raw;
	alp_dsp_stats_f32(st->samples, (size_t)n, &raw);
	const float mean = raw.mean;

	for (int i = 0; i < n; i++) {
		xc[i] = st->samples[i] - mean;
	}
	for (int i = n; i < ACO_FRAME_N; i++) {
		xc[i] = 0.0f; /* zero-pad the FFT tail on a short frame */
	}

	alp_dsp_stats_t ac;
	alp_dsp_stats_f32(xc, (size_t)n, &ac);
	float peak     = ac.abs_max;  /* peak |xc|                       */
	float var      = ac.variance; /* == mean(xc^2) since mean(xc)~=0 */
	out->total_rms = ac.rms;      /* AC RMS = sqrt(mean(xc^2))       */

	/*
	 * Kurtosis (4th standardised central moment): CMSIS-DSP ships no
	 * 4th-moment kernel, so the sum-of-4th-powers stays a portable loop
	 * over xc -- alp_dsp_stats_f32 covers mean/RMS/abs-peak/variance above,
	 * not this.
	 *   kurtosis = E[(x-mu)^4] / (E[(x-mu)^2])^2 = (sum4/n) / var^2
	 *   Gaussian noise gives kurtosis ~= 3; impulsive fault signatures
	 *   such as rolling-element bearing spall impacts push it above 10.
	 */
	float sum4 = 0.0f;
	for (int i = 0; i < n; i++) {
		float x2 = xc[i] * xc[i];
		sum4 += x2 * x2;
	}
	out->kurtosis = (var > 1e-12f) ? ((sum4 / (float)n) / (var * var)) : 0.0f;

	/*
	 * ----- Sub-pass B: frequency-domain statistics via the portable
	 * <alp/dsp.h> chain (NOT a hand-rolled FFT). ---------------------------
	 *
	 * A single ALP_DSP_STAGE_FFT (rectangular, no window) transforms the
	 * DC-removed frame to magnitude bins.  The backend runs CMSIS-DSP
	 * arm_rfft_fast_f32 on the M55 and a portable-C radix-2 FFT under
	 * native_sim -- the example source is identical either way.
	 *
	 * The chain consumes int16 samples (the codec's native PCM format).
	 * We scale the float frame to fill the int16 range before feeding it:
	 * the absolute scale is irrelevant here because every downstream
	 * spectral feature below (centroid, flatness, band energies) is
	 * ratio-based and therefore scale-invariant.
	 *
	 * The FFT of ACO_FRAME_N (= 256) points gives 128 positive-frequency
	 * bins; bin k corresponds to frequency f_k = k * sr_hz / ACO_FRAME_N.
	 * Bin 0 (DC) is discarded; only bins 1..half-1 are used.
	 *
	 * Spectral centroid:
	 *   f_c = sum_k(f_k * |X_k|) / sum_k(|X_k|)
	 *   The magnitude-weighted mean frequency rises when energy shifts to higher
	 *   harmonics, e.g. under gear-mesh wear or bearing defects.
	 *
	 * Spectral flatness (Wiener entropy):
	 *   SF = geometric_mean(|X|) / arithmetic_mean(|X|)
	 *      = exp( mean(log(|X_k| + eps)) ) / mean(|X_k| + eps)
	 *   SF ≈ 1 for broadband / white-noise excitation; SF → 0 as the spectrum
	 *   concentrates on a pure tone.  A turbine with broken blade develops a
	 *   strong tonal signature, pulling SF below the healthy baseline.
	 *   A small epsilon (1e-9) floors each magnitude to avoid log(0).
	 * --------------------------------------------------------------------------
	 */
	static int16_t samp_q15[ACO_FRAME_N];
	float          scale = (peak > 1e-9f) ? (30000.0f / peak) : 0.0f;
	for (int i = 0; i < ACO_FRAME_N; i++) {
		samp_q15[i] = (int16_t)lrintf(xc[i] * scale);
	}

	static float    mag[ACO_FRAME_N];
	alp_dsp_stage_t stages[] = {
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points = ACO_FRAME_N, .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE } },
	};
	alp_dsp_chain_t *chain = alp_dsp_chain_open(stages, 1u);
	size_t           got   = 0;
	memset(mag, 0, sizeof(mag));
	if (chain != NULL) {
		(void)alp_dsp_chain_apply_bins(chain, samp_q15, ACO_FRAME_N, mag, ACO_FRAME_N, &got);
		alp_dsp_chain_close(chain);
	}

	/* half = N/2 = 128: positive-frequency bin count; mag[] (from the
	 * chain above) stores the single-sided magnitudes used by both the
	 * spectral stats and band C.  mag_total/centroid_num: centroid
	 * denominator and numerator respectively.  log_sum/lin_sum: per-bin
	 * log and linear accumulations for flatness. */
	const int half      = ACO_FRAME_N / 2;
	float     mag_total = 0.0f, centroid_num = 0.0f;
	float     log_sum = 0.0f, lin_sum = 0.0f;
	int       active = 0;
	for (int k = 1; k < half; k++) {
		float m = mag[k];
		float f = (float)k * sr_hz / (float)ACO_FRAME_N;
		mag_total += m;
		centroid_num += f * m;
		/* Spectral flatness on a small-epsilon floored magnitude. */
		float me = m + 1e-9f;
		log_sum += logf(me);
		lin_sum += me;
		active++;
	}
	/* Derive centroid and flatness from the per-bin accumulators computed above. */
	out->spectral_centroid_hz = (mag_total > 1e-12f) ? (centroid_num / mag_total) : 0.0f;
	if (active > 0 && lin_sum > 1e-12f) {
		float geo              = expf(log_sum / (float)active);
		float arith            = lin_sum / (float)active;
		out->spectral_flatness = geo / arith;
	} else {
		out->spectral_flatness = 0.0f;
	}

	/* ----- Sub-pass C: log-spaced band energies ------------------------------
	 *
	 * Human hearing -- and turbine fault signatures -- scale logarithmically in
	 * frequency.  Each FFT bin k (1..half-1) is mapped to a band index via:
	 *
	 *     band = floor( log(k) / log(half) * ACO_N_BANDS )
	 *
	 * This distributes the 127 positive bins into ACO_N_BANDS (12) equal-ratio
	 * bands.  Bin k=1 always maps to band 0; bin k=half-1 ≈ band 11.
	 *
	 * Energy is accumulated as |X_k|^2 (power spectrum), consistent with
	 * Parseval's theorem.  The band sums are then divided by the total spectral
	 * power so that band_energy[] sums to 1.0 regardless of signal level,
	 * making the feature invariant to microphone gain and rotor distance.
	 *
	 * A near-zero total (silent / clipped frame) returns early; all outputs
	 * remain at the zeroed-by-memset sentinel.
	 * --------------------------------------------------------------------------
	 */
	/* Accumulate total spectral power; used as the normalisation divisor. */
	float mag2_total = 0.0f;
	for (int k = 1; k < half; k++) {
		mag2_total += mag[k] * mag[k];
	}
	if (mag2_total < 1e-20f) {
		return;
	}
	for (int k = 1; k < half; k++) {
		/* pos in [0,1) maps linearly to band index on the log-frequency axis. */
		float pos = logf((float)k) / logf((float)half);
		int   b   = (int)(pos * (float)ACO_N_BANDS);
		if (b < 0) {
			b = 0;
		}
		if (b >= ACO_N_BANDS) {
			b = ACO_N_BANDS - 1;
		}
		out->band_energy[b] += mag[k] * mag[k];
	}
	/* Normalise each band by total power -> unit-sum energy fraction. */
	for (int b = 0; b < ACO_N_BANDS; b++) {
		out->band_energy[b] /= mag2_total;
	}
}

/* Pack all features into a flat float vector in the order expected by the
 * downstream model (or aco_anomaly_fallback): band_energy[0..11], then
 * spectral_flatness, spectral_centroid_hz, kurtosis, total_rms.
 * Returns the number of elements written (== ACO_FEATURE_DIM) or 0 if cap
 * is too small. */
size_t aco_feat_pack(const struct aco_features *f, float *vec, size_t cap)
{
	if (cap < (size_t)ACO_FEATURE_DIM) {
		return 0;
	}
	size_t i = 0;
	for (int b = 0; b < ACO_N_BANDS; b++) {
		vec[i++] = f->band_energy[b];
	}
	/* Scalar features appended in the order the downstream model expects them. */
	vec[i++] = f->spectral_flatness;
	vec[i++] = f->spectral_centroid_hz;
	vec[i++] = f->kurtosis;
	vec[i++] = f->total_rms;
	return i; /* == ACO_FEATURE_DIM */
}

/* ---------------------------------------------------------------------------
 * aco_anomaly_fallback -- diagonal Mahalanobis anomaly score.
 *
 * This is the CPU-only fallback used when no trained NPU model is loaded.
 * It compares the current feature vector against a stored healthy-operation
 * baseline (per-feature mean and inverse variance) and returns a bounded
 * anomaly score.
 *
 * Step 1 -- Squared diagonal Mahalanobis distance:
 *
 *     d^2 = sum_i [ (x_i - mu_i)^2 * inv_var_i ]
 *
 *   The diagonal approximation treats features as independent.  inv_var_i =
 *   1/sigma_i^2 scales each dimension by its variability in normal operation,
 *   so features with tight healthy distributions contribute more to the score.
 *
 * Step 2 -- Squash to [0, 1):
 *
 *     score = 1 - exp(-d^2 / ACO_FEATURE_DIM)
 *
 *   Dividing by ACO_FEATURE_DIM normalises the exponent to a per-feature
 *   average Mahalanobis deviation, making the curve independent of vector
 *   length.  score → 0 when d^2 ≈ 0 (healthy match); score → 1 as
 *   deviation grows large.  The hard clamp [0,1] handles any float edge cases.
 * ---------------------------------------------------------------------------
 */
float aco_anomaly_fallback(const float *vec, size_t n, const struct aco_baseline *base)
{
	float d2 = 0.0f;
	for (size_t i = 0; i < n; i++) {
		float dx = vec[i] - base->mean[i];
		d2 += dx * dx * base->inv_var[i];
	}
	/* Squash to [0,1): grows with normalised distance, saturates smoothly. */
	float score = 1.0f - expf(-d2 / (float)ACO_FEATURE_DIM);
	if (score < 0.0f) {
		score = 0.0f;
	}
	if (score > 1.0f) {
		score = 1.0f;
	}
	return score;
}
