/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * acoustic_features implementation -- see acoustic_features.h.
 */
#include "acoustic_features.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

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
 * fft_radix2 -- in-place iterative Cooley-Tukey DIT (decimation-in-time) FFT.
 *
 * Prerequisites: n must be a power of two; re[] and im[] hold the real and
 * imaginary parts on entry and the complex DFT spectrum on exit.
 *
 * The algorithm consists of two sequential passes:
 *
 * PASS 1 -- Bit-reversal permutation.
 *   Cooley-Tukey DIT requires the input in bit-reversed index order before
 *   the butterfly stages begin.  The permutation is computed in-place with a
 *   single O(n) scan using a second index j that tracks the bit-reversed
 *   counterpart of i.  j is advanced with the standard Gray-code trick:
 *   walk a bitmask from the MSB downward, XOR-clearing each set bit in j
 *   until the first unset bit is found, then XOR-set it.  Whenever i < j the
 *   elements at those positions are swapped (each pair is touched once).
 *
 * PASS 2 -- Butterfly stages (log2(n) passes, stage width 'len').
 *   Each stage decomposes the DFT of length 'len' into two DFTs of length
 *   'len/2'.  For butterfly index k within a group starting at i:
 *
 *       a = i + k,   b = i + k + len/2
 *       T = W^k * X[b]           (complex multiply by twiddle factor)
 *       X[b] = X[a] - T
 *       X[a] = X[a] + T
 *
 *   where W^k = exp(-j * 2*pi*k / len).  Instead of calling cosf/sinf for
 *   every butterfly, the twiddle is advanced with a single complex multiply
 *   per step (the recurrence W^(k+1) = W^k * W^1), costing 4 multiplies +
 *   2 adds rather than two transcendental calls.
 * ---------------------------------------------------------------------------
 */
static void fft_radix2(float *re, float *im, int n)
{
	/* --- Pass 1: bit-reversal permutation --- */
	for (int i = 1, j = 0; i < n; i++) {
		/* Advance j to its next bit-reversed value.
		 * Walk 'bit' from the MSB; XOR-clear each already-set bit until
		 * we reach the first unset position, then XOR-set it. */
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			/* Swap complex samples at i and j (each unordered pair once). */
			float tr = re[i];
			re[i]    = re[j];
			re[j]    = tr;
			float ti = im[i];
			im[i]    = im[j];
			im[j]    = ti;
		}
	}

	/* --- Pass 2: log2(n) butterfly stages --- */
	for (int len = 2; len <= n; len <<= 1) {
		/* Unit twiddle step for this stage: W_step = exp(-j*2*pi/len).
		 * Computed once per stage; used to advance W^k by one position. */
		float ang = -2.0f * (float)M_PI / (float)len;
		float wlr = cosf(ang);
		float wli = sinf(ang);
		for (int i = 0; i < n; i += len) {
			/* Running twiddle W^k, initialised to W^0 = 1 + j*0. */
			float wr = 1.0f, wi = 0.0f;
			for (int k = 0; k < len / 2; k++) {
				int a = i + k;
				int b = i + k + len / 2;
				/* T = W^k * X[b]  (complex multiply). */
				float tr = wr * re[b] - wi * im[b];
				float ti = wr * im[b] + wi * re[b];
				/* Radix-2 butterfly: X[a] += T, X[b] = X[a] - T. */
				re[b] = re[a] - tr;
				im[b] = im[a] - ti;
				re[a] += tr;
				im[a] += ti;
				/* Twiddle recurrence: W^(k+1) = W^k * W_step. */
				float nwr = wr * wlr - wi * wli;
				wi        = wr * wli + wi * wlr;
				wr        = nwr;
			}
		}
	}
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
	float mean = 0.0f;
	for (int i = 0; i < n; i++) {
		mean += st->samples[i];
	}
	mean /= (float)n;

	/* sum2 = sum of squared deviations; sum4 = sum of fourth-power deviations. */
	float sum2 = 0.0f, sum4 = 0.0f;
	for (int i = 0; i < n; i++) {
		float x = st->samples[i] - mean;
		sum2 += x * x;
		sum4 += x * x * x * x;
	}
	float var      = sum2 / (float)n;
	out->total_rms = sqrtf(var);
	out->kurtosis  = (var > 1e-12f) ? ((sum4 / (float)n) / (var * var)) : 0.0f;

	/* ----- Sub-pass B: frequency-domain statistics ---------------------------
	 *
	 * The DC-removed samples are placed in re[]; any remaining positions up to
	 * ACO_FRAME_N are zero-padded (valid for a DFT on the available data).
	 * The FFT of ACO_FRAME_N (= 256) points gives 128 positive-frequency bins;
	 * bin k corresponds to frequency f_k = k * sr_hz / ACO_FRAME_N.
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
	/* Static scratch buffers avoid VLA stack pressure on Cortex-M; they are
	 * safe here because aco_feat_extract is not re-entrant (single codec path).
	 * im[] is zeroed explicitly each call; re[] is fully overwritten below. */
	static float re[ACO_FRAME_N];
	static float im[ACO_FRAME_N];
	for (int i = 0; i < ACO_FRAME_N; i++) {
		re[i] = (i < n) ? (st->samples[i] - mean) : 0.0f;
		im[i] = 0.0f;
	}
	/* Run the in-place DIT FFT; re[]/im[] become the complex DFT output. */
	fft_radix2(re, im, ACO_FRAME_N);

	/* half = N/2 = 128: positive-frequency bin count; mag[] stores the
	 * single-sided magnitudes used by both the spectral stats and band C.
	 * mag_total/centroid_num: centroid denominator and numerator respectively.
	 * log_sum/lin_sum: per-bin log and linear accumulations for flatness. */
	const int half = ACO_FRAME_N / 2;
	float     mag[ACO_FRAME_N / 2];
	float     mag_total = 0.0f, centroid_num = 0.0f;
	float     log_sum = 0.0f, lin_sum = 0.0f;
	int       active = 0;
	for (int k = 1; k < half; k++) {
		float m = sqrtf(re[k] * re[k] + im[k] * im[k]);
		mag[k]  = m;
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
