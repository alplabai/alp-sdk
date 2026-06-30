/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rail_features implementation -- see rail_features.h.
 */
#include "rail_features.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/*
 * rail_feat_state_reset
 *
 * The samples[] array is intentionally NOT zeroed: every push overwrites
 * entries starting at index 0, and rail_feat_extract reads only the first
 * st->count elements.  Clearing just the counter avoids writing 1 kB of
 * data (RAIL_WINDOW_N * sizeof(float) = 256 * 4 bytes) on every reset.
 */
void rail_feat_state_reset(struct rail_feat_state *st)
{
	st->count = 0;
}

/*
 * rail_feat_window_push
 *
 * Append one vibration-magnitude sample to the sliding window.  Samples
 * are expected at RAIL_ODR_HZ (800 Hz) from the IMU output stage.  Once
 * the window is full (count == RAIL_WINDOW_N) pushes are silently dropped;
 * the caller must call rail_feat_extract then rail_feat_state_reset before
 * resuming collection.
 */
void rail_feat_window_push(struct rail_feat_state *st, float sample)
{
	if (st->count < RAIL_WINDOW_N) {
		st->samples[st->count++] = sample;
	}
}

/*
 * rail_feat_window_full
 *
 * Predicate: returns true once RAIL_WINDOW_N samples have accumulated.
 * The caller polls this after each push to decide when to trigger feature
 * extraction without tracking the counter externally.
 */
bool rail_feat_window_full(const struct rail_feat_state *st)
{
	return st->count >= RAIL_WINDOW_N;
}

/*
 * fft_radix2 -- in-place iterative radix-2 DIT Cooley-Tukey FFT.
 *
 * MATHEMATICAL BACKGROUND
 * -----------------------
 * The N-point Discrete Fourier Transform is defined as:
 *
 *   X[k] = SUM_{n=0}^{N-1} x[n] * W_N^{nk},   W_N = e^{-j*2*pi/N}
 *
 * Direct evaluation of all N output bins costs O(N^2) complex
 * multiplications.  The Cooley-Tukey factorisation cuts this to
 * O(N * log2(N)) by exploiting the half-cycle symmetry of the twiddle
 * factors:
 *
 *   W_N^{k + N/2} = -W_N^k   (180-degree phase shift = sign flip)
 *
 * This lets each N-point DFT be split into two N/2-point DFTs and
 * recombined with one butterfly operation per output pair.
 *
 * DECIMATION-IN-TIME (DIT) VARIANT
 * ----------------------------------
 * DIT splits the input by even/odd index at each level of recursion.
 * The recursion is unrolled into log2(N) sequential butterfly passes
 * over the full array (iterative Gentleman-Sande formulation), which
 * avoids all recursive stack frames -- important for the M55 default
 * stack size.
 *
 * For N = RAIL_WINDOW_N = 256: log2(256) = 8 butterfly passes.
 *
 * PARAMETERS
 *   re[0..n-1]  real parts on entry (time-domain signal); on return,
 *               the real parts of X[0..n-1].
 *   im[0..n-1]  imaginary parts on entry (zero for a real signal); on
 *               return, the imaginary parts of X[0..n-1].
 *   n           transform length; MUST be a power of 2.
 */
/* In-place iterative radix-2 FFT, N = RAIL_WINDOW_N, re/im length N. */
static void fft_radix2(float *re, float *im, int n)
{
	/*
	 * STEP 1: Bit-reversal permutation.
	 *
	 * DIT Cooley-Tukey requires the input in bit-reversed index order
	 * before the butterfly passes begin.  Example for N = 8:
	 *
	 *   natural order:  0  1  2  3  4  5  6  7
	 *   bit-reversed:   0  4  2  6  1  5  3  7
	 *
	 * For N = 256 (8-bit index), index 3 (0000 0011b) maps to 192
	 * (1100 0000b).
	 *
	 * The loop maintains j as the running bit-reversal of i using a
	 * carry-propagation trick (no lookup table):
	 *   - 'bit' starts at the MSB of the n-element index space.
	 *   - While j has 'bit' set: clear it, shift 'bit' right (propagate
	 *     the carry downward through the reversed index representation).
	 *   - Finally set 'bit' in j to advance it to bit_reverse(i+1).
	 * Swapping re[i] with re[j] (and im[i] with im[j]) only when i < j
	 * guarantees each pair is exchanged exactly once.
	 */
	/* Bit-reversal permutation. */
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			float tr = re[i];
			re[i]    = re[j];
			re[j]    = tr;
			float ti = im[i];
			im[i]    = im[j];
			im[j]    = ti;
		}
	}
	/*
	 * STEP 2: Butterfly passes (stages s = 1 .. log2(n)).
	 *
	 * At stage s (len = 2^s), the array is partitioned into n/len
	 * blocks of len elements each.  Each block is the result of
	 * combining two adjacent DFTs of size len/2 (already computed in
	 * previous stages) into one DFT of size len using the radix-2
	 * butterfly for k = 0 .. len/2 - 1:
	 *
	 *   A'[k] = A[k] + W_len^k * B[k]          (upper output)
	 *   B'[k] = A[k] - W_len^k * B[k]          (lower output)
	 *
	 * where A and B are the lower and upper half of the block, and
	 * W_len^k = e^{-j*2*pi*k/len} is the twiddle factor for bin k.
	 *
	 * TWIDDLE RECURRENCE
	 * ------------------
	 * Calling cosf/sinf inside the inner butterfly loop is expensive
	 * (on M55 without an FP sin/cos instruction each call is ~50+
	 * cycles).  Instead, the unit-circle phasor [wr + j*wi] is
	 * stepped by one angular increment per butterfly via complex
	 * multiplication by the per-stage base phasor:
	 *
	 *   [wlr + j*wli] = [cos(-2*pi/len), sin(-2*pi/len)]
	 *
	 * Recurrence:
	 *   wr' = wr*wlr - wi*wli
	 *   wi' = wr*wli + wi*wlr
	 *
	 * [wlr, wli] is computed once per stage with cosf/sinf.  Over
	 * 128 steps (the largest half-block at N=256) the accumulated
	 * rounding error stays below 1-2 ULP.
	 */
	for (int len = 2; len <= n; len <<= 1) {
		float ang = -2.0f * (float)M_PI / (float)len;
		float wlr = cosf(ang);
		float wli = sinf(ang);
		for (int i = 0; i < n; i += len) {
			float wr = 1.0f, wi = 0.0f;
			for (int k = 0; k < len / 2; k++) {
				/*
				 * Butterfly: a and b are the lower/upper indices.
				 *   tr = real(W*B[k]) = wr*re[b] - wi*im[b]
				 *   ti = imag(W*B[k]) = wr*im[b] + wi*re[b]
				 * Then:  A'[k] = A[k] + W*B[k]  (overwrite re[a])
				 *        B'[k] = A[k] - W*B[k]  (overwrite re[b])
				 */
				int   a  = i + k;
				int   b  = i + k + len / 2;
				float tr = wr * re[b] - wi * im[b];
				float ti = wr * im[b] + wi * re[b];
				re[b]    = re[a] - tr;
				im[b]    = im[a] - ti;
				re[a] += tr;
				im[a] += ti;
				/* Advance twiddle phasor by one angular step. */
				float nwr = wr * wlr - wi * wli;
				wi        = wr * wli + wi * wlr;
				wr        = nwr;
			}
		}
	}
}

/*
 * rail_feat_extract -- reduce one vibration window to a feature vector.
 *
 * PIPELINE OVERVIEW
 * -----------------
 * 1. Subtract the window mean to remove DC (gravity bias, sensor offset).
 * 2. Compute time-domain statistics: RMS, peak absolute value, kurtosis.
 * 3. Zero-pad the mean-centred signal to RAIL_WINDOW_N and run the FFT.
 * 4. Find the dominant spectral bin (peak magnitude-squared, DC skipped).
 * 5. Convert dominant bin to Hz; from Hz and speed, derive wavelength.
 * 6. Accumulate log-spaced spectral band energies; normalise to sum = 1.
 *
 * The feature vector produced here feeds both the deterministic fallback
 * classifier (rail_classify_fallback) and the AI runtime when a model
 * has been loaded over OTA.
 */
void rail_feat_extract(const struct rail_feat_state *st,
                       float                         odr_hz,
                       float                         speed_mps,
                       struct rail_features         *out)
{
	const int n = (st->count < RAIL_WINDOW_N) ? st->count : RAIL_WINDOW_N;

	memset(out, 0, sizeof(*out));
	if (n <= 0) {
		return;
	}

	/* Mean (DC) removal. */
	float mean = 0.0f;
	for (int i = 0; i < n; i++) {
		mean += st->samples[i];
	}
	mean /= (float)n;

	/*
	 * TIME-DOMAIN STATISTICS
	 * ----------------------
	 *
	 * All statistics operate on x[i] = sample[i] - mean, i.e. the
	 * AC (vibration) component with the DC bias removed.
	 *
	 * RMS (root-mean-square)
	 *   rms = sqrt( (1/n) * SUM x[i]^2 ) = sqrt(E[x^2])
	 *   Measures broadband vibration energy.  For a pure sine of
	 *   amplitude A: rms = A / sqrt(2) ~= 0.707 * A.  Used as the
	 *   primary energy indicator for ROUGH_RCF classification.
	 *
	 * Crest factor  (CF = peak / rms)
	 *   For band-limited Gaussian noise CF ~= 3-4.  A single-cycle
	 *   impulse (wheel flat, joint impact) drives CF to 10-20 because
	 *   the transient spike inflates peak far more than RMS, which is
	 *   averaged over the whole window.  Guard: rms < 1e-9 clamps CF
	 *   to 0 to prevent division by a near-zero denominator.
	 *
	 * Kurtosis  (K = E[x^4] / E[x^2]^2)
	 *   The 4th standardised central moment.  Gaussian noise gives
	 *   K ~= 3 (mesokurtic).  Impulsive events push K well above 5
	 *   because the 4th power amplifies rare large deviations far
	 *   more than the variance term in the denominator.  Used with
	 *   crest factor as the dual criterion for JOINT_WELD detection.
	 *   Guard: var < 1e-12 (zero signal) clamps K to 0.
	 */
	/* Time-domain moments: RMS, peak, kurtosis. */
	float sum2 = 0.0f, peak = 0.0f, sum4 = 0.0f;
	for (int i = 0; i < n; i++) {
		float x  = st->samples[i] - mean;
		float ax = fabsf(x);
		sum2 += x * x;
		sum4 += x * x * x * x;
		if (ax > peak) {
			peak = ax;
		}
	}
	float var         = sum2 / (float)n;
	out->rms          = sqrtf(var);
	out->crest_factor = (out->rms > 1e-9f) ? (peak / out->rms) : 0.0f;
	out->kurtosis     = (var > 1e-12f) ? ((sum4 / (float)n) / (var * var)) : 0.0f;

	/*
	 * SPECTRAL ANALYSIS
	 * -----------------
	 *
	 * The FFT always runs on RAIL_WINDOW_N = 256 points.  If the
	 * window holds fewer than 256 samples (session edge or partial
	 * window), the remaining entries are zero-padded.  Zero-padding
	 * does not add spectral energy; it refines the frequency resolution
	 * by interpolating the existing spectrum to finer bin spacing.
	 *
	 * Static local buffers: feature extraction is called from a single
	 * sensor processing thread; no re-entrancy, so static is safe and
	 * avoids a 2 kB VLA on the call stack.
	 */
	/* Spectrum over a fixed RAIL_WINDOW_N FFT (zero-pad a short window). */
	static float re[RAIL_WINDOW_N];
	static float im[RAIL_WINDOW_N];
	for (int i = 0; i < RAIL_WINDOW_N; i++) {
		re[i] = (i < n) ? (st->samples[i] - mean) : 0.0f;
		im[i] = 0.0f;
	}
	fft_radix2(re, im, RAIL_WINDOW_N);

	/*
	 * DOMINANT FREQUENCY BIN
	 * ----------------------
	 *
	 * For a real N-point signal sampled at fs Hz, the FFT produces a
	 * two-sided spectrum; the physically meaningful positive-frequency
	 * content occupies bins 0 .. N/2.  Bin k represents:
	 *
	 *   f_k = k * fs / N    [Hz]
	 *
	 * The search starts at k = 1 to skip bin 0 (DC).  Even after
	 * mean removal a floating-point residual can linger in bin 0,
	 * and the relevant vibration content always starts at k >= 1.
	 */
	/* Magnitude-squared over the first half; find the dominant bin. */
	const int half = RAIL_WINDOW_N / 2;
	float     mag2[RAIL_WINDOW_N / 2];
	int       dom_bin = 1;
	float     dom_val = -1.0f;
	for (int k = 0; k < half; k++) {
		mag2[k] = re[k] * re[k] + im[k] * im[k];
		if (k >= 1 && mag2[k] > dom_val) { /* skip DC bin 0 */
			dom_val = mag2[k];
			dom_bin = k;
		}
	}
	/*
	 * FREQUENCY-TO-WAVELENGTH CONVERSION
	 * ------------------------------------
	 *
	 * A train travelling at speed v [m/s] over a corrugation of
	 * spatial period lambda [m] excites sinusoidal vibration at:
	 *
	 *   f = v / lambda   [Hz]
	 *
	 * Inverting: lambda = v / f.
	 *
	 * Short-pitch rail corrugation has a FIXED spatial wavelength
	 * (typically 20-300 mm) that is a property of the rail surface,
	 * independent of train speed.  Because the observed frequency f
	 * scales with speed, a raw frequency reading cannot be compared
	 * across trains running at different speeds.  The wavelength
	 * lambda = v/f is the speed-invariant descriptor that identifies
	 * a corrugation's physical defect length regardless of how fast
	 * the measuring vehicle travels.
	 *
	 * Guards: both dom_freq_hz and speed_mps must exceed 1e-6; when
	 * either is near zero (train at standstill, or no spectral peak),
	 * wavelength is set to 0.0 to signal "not available".
	 */
	out->dom_freq_hz = (float)dom_bin * odr_hz / (float)RAIL_WINDOW_N;
	out->rail_wavelength_m =
	    (out->dom_freq_hz > 1e-6f && speed_mps > 1e-6f) ? (speed_mps / out->dom_freq_hz) : 0.0f;

	/*
	 * LOG-SPACED SPECTRAL BAND ENERGIES
	 * -----------------------------------
	 *
	 * The positive-frequency bins 1 .. N/2-1 are accumulated into
	 * RAIL_N_BANDS = 8 bands with logarithmically-spaced boundaries.
	 *
	 * Why log-spacing?
	 *   (a) Rail defect excitation spans several decades: ~10 Hz for
	 *       long-pitch corrugation to ~400 Hz for short-pitch.
	 *   (b) Linear equal-bandwidth bands devote the majority of bins
	 *       to high frequencies where defect energy is low, wasting
	 *       diagnostic resolution where defects actually appear.
	 *   (c) Log-spacing mirrors the 1/f^alpha character of vibration
	 *       spectra, giving equal diagnostic weight to each decade.
	 *
	 * Assignment rule: bin k maps to band:
	 *   b = floor( log(k) / log(N/2) * B )
	 * where B = RAIL_N_BANDS.  log(1)=0 -> band 0; approaching
	 * log(N/2-1) ~= log(N/2) -> band B-1.
	 *
	 * Normalisation: each band energy is divided by the total spectral
	 * energy so that the feature vector is amplitude-scale-invariant --
	 * a heavier train produces the same band pattern for the same
	 * defect type, regardless of absolute vibration level.
	 */
	/* Log-spaced band energies over bins 1..half-1, normalised to sum 1. */
	float total = 0.0f;
	for (int k = 1; k < half; k++) {
		total += mag2[k];
	}
	if (total < 1e-20f) {
		return; /* bands stay zero */
	}
	for (int k = 1; k < half; k++) {
		/* Map bin -> band by log position across [1, half). */
		float pos = logf((float)k) / logf((float)half);
		int   b   = (int)(pos * (float)RAIL_N_BANDS);
		if (b < 0) {
			b = 0;
		}
		if (b >= RAIL_N_BANDS) {
			b = RAIL_N_BANDS - 1;
		}
		out->band_energy[b] += mag2[k];
	}
	for (int b = 0; b < RAIL_N_BANDS; b++) {
		out->band_energy[b] /= total;
	}
}

/*
 * rail_feat_pack -- serialise features to a flat float vector.
 *
 * Output layout (RAIL_FEATURE_DIM = 3 + RAIL_N_BANDS + 2 = 13 floats):
 *   [0]      rms
 *   [1]      crest_factor
 *   [2]      kurtosis
 *   [3..10]  band_energy[0..7]   (log-spaced, normalised, sum to 1)
 *   [11]     dom_freq_hz
 *   [12]     rail_wavelength_m
 *
 * This exact layout is the input contract for the companion .alpmodel
 * classifier; reordering the fields requires retraining the model.
 */
size_t rail_feat_pack(const struct rail_features *f, float *vec, size_t cap)
{
	if (cap < (size_t)RAIL_FEATURE_DIM) {
		return 0;
	}
	size_t i = 0;
	vec[i++] = f->rms;
	vec[i++] = f->crest_factor;
	vec[i++] = f->kurtosis;
	for (int b = 0; b < RAIL_N_BANDS; b++) {
		vec[i++] = f->band_energy[b];
	}
	vec[i++] = f->dom_freq_hz;
	vec[i++] = f->rail_wavelength_m;
	return i; /* == RAIL_FEATURE_DIM */
}

/*
 * rail_classify_fallback -- deterministic rule-based classifier.
 *
 * PURPOSE
 * -------
 * When no trained .alpmodel is available (native_sim builds, or first
 * boot before OTA model delivery), this function produces physically
 * meaningful verdicts from the feature vector without any floating-point
 * weight tables.  It covers all four taxonomy classes and is calibrated
 * to match the AI path on unambiguous cases.
 *
 * BRANCH ORDER
 * ------------
 * The conditions can overlap at boundary values, so the more specific
 * and physically severe defect class is tested first.  A signal that
 * qualifies for multiple classes (e.g., a rough weld with a narrowband
 * spectral peak) is assigned the most severe matching class.
 *
 * 1. JOINT_WELD  (crest_factor > 6 AND kurtosis > 5)
 *    A rail joint or weld produces a discrete transient: a brief,
 *    high-amplitude spike that dominates the window.  This spike
 *    simultaneously drives crest factor (spike/RMS ratio) and kurtosis
 *    (4th-moment sensitivity to rare large deviations) above their
 *    respective thresholds.  The dual criterion suppresses false
 *    positives: corrugation alone rarely elevates both features at
 *    the same time.
 *    Severity: linear from 0 at CF = 6.0 to 1.0 at CF = 12.0.
 *    Checked FIRST because a severe weld may also look narrowband
 *    (high band_max), but the dual criterion catches it here.
 *
 * 2. CORRUGATION  (band_max > 0.5)
 *    Rail corrugation is a quasi-periodic surface wave; the wheel
 *    rolls over peaks and troughs at a near-fixed spatial wavelength,
 *    producing near-sinusoidal excitation concentrated in one frequency
 *    band.  When the strongest log-band holds more than 50 % of total
 *    spectral energy the spectrum is "narrowband" -- the definitive
 *    corrugation signature.
 *    Severity: band_max itself (already in [0.5, 1], clipped to 1).
 *    Checked after JOINT_WELD, which already handles the overlap with
 *    the dual criterion above.
 *
 * 3. ROUGH_RCF  (rms > 0.30)
 *    Rolling Contact Fatigue (RCF) develops as micro-cracks and
 *    spalling across a wide track section.  The resulting vibration
 *    is broadband (no single dominant frequency), but the overall
 *    energy level is elevated.  RMS > 0.30 m/s^2 (tuned for an ODR
 *    of 800 Hz and a +/-2 g accelerometer range) flags significant
 *    broadband roughness.
 *    Severity: linear from 0 at rms = 0.30 to 1.0 at rms = 0.60.
 *    Checked after CORRUGATION: corrugated track also has elevated RMS,
 *    but the narrowband test already classified it at step 2.
 *
 * 4. HEALTHY  (fallthrough)
 *    Low RMS, no narrowband spectral peak, no impulsive transient.
 *    Severity is 0.0 (no defect energy detected).
 */
struct rail_verdict rail_classify_fallback(const struct rail_features *f)
{
	struct rail_verdict v = { RAIL_HEALTHY, 0.0f };

	/* Narrowband ratio: fraction of spectral energy in the strongest band. */
	float band_max = 0.0f;
	for (int b = 0; b < RAIL_N_BANDS; b++) {
		if (f->band_energy[b] > band_max) {
			band_max = f->band_energy[b];
		}
	}

	if (f->crest_factor > 6.0f && f->kurtosis > 5.0f) {
		v.cls      = RAIL_JOINT_WELD;
		v.severity = fminf(1.0f, (f->crest_factor - 6.0f) / 6.0f);
	} else if (band_max > 0.5f) {
		v.cls      = RAIL_CORRUGATION;
		v.severity = fminf(1.0f, band_max);
	} else if (f->rms > 0.30f) {
		v.cls      = RAIL_ROUGH_RCF;
		v.severity = fminf(1.0f, (f->rms - 0.30f) / 0.30f);
	} else {
		v.cls      = RAIL_HEALTHY;
		v.severity = 0.0f;
	}
	return v;
}

/*
 * rail_class_name -- stable string representation of a rail_class_t.
 *
 * Returns a NUL-terminated upper-case ASCII literal in static storage;
 * the caller must not free or mutate it.  Used to embed class labels in
 * CSV output records and structured log lines.
 */
const char *rail_class_name(rail_class_t c)
{
	switch (c) {
	case RAIL_HEALTHY:
		return "HEALTHY";
	case RAIL_CORRUGATION:
		return "CORRUGATION";
	case RAIL_JOINT_WELD:
		return "JOINT_WELD";
	case RAIL_ROUGH_RCF:
		return "ROUGH_RCF";
	default:
		return "UNKNOWN";
	}
}
