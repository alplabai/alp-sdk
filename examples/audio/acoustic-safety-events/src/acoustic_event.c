/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * acoustic_event implementation -- see acoustic_event.h.
 */
#include "acoustic_event.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Reset the accumulator so the next push starts at index 0. */
void ase_frame_reset(struct ase_frame_state *st)
{
	st->count = 0;
}

/* Append one sample; silently ignore pushes past ASE_FRAME_N. */
void ase_frame_push(struct ase_frame_state *st, float sample)
{
	if (st->count < ASE_FRAME_N) {
		st->samples[st->count++] = sample;
	}
}

/* True once the accumulator holds a full ASE_FRAME_N samples. */
bool ase_frame_full(const struct ase_frame_state *st)
{
	return st->count >= ASE_FRAME_N;
}

/*
 * fft_radix2 -- in-place iterative Cooley-Tukey radix-2 DIT FFT.
 *
 * ALGORITHM OVERVIEW
 * ------------------
 * The N-point Discrete Fourier Transform is:
 *
 *   X[k] = Σ_{n=0}^{N-1}  x[n] · e^{-j2πkn/N}        k = 0 … N-1
 *
 * Naïve evaluation costs O(N²) multiply-adds.  The Cooley-Tukey DIT
 * (decimation-in-time) factorisation recursively splits the input into
 * even- and odd-indexed subsequences, reducing the cost to O(N log₂ N).
 *
 * BIT-REVERSAL PERMUTATION
 * ------------------------
 * The DIT recursion reorders inputs so that x[0], x[N/2], x[N/4], x[3N/4],
 * … arrive in bit-reversed index order before the first butterfly stage.
 * Rather than recurse, the loop below computes the permutation iteratively
 * using a carry-ripple trick: j tracks the bit-reversed position of i, and
 * is updated each step by toggling the top-most bit not yet carried, which
 * is equivalent to adding 1 in bit-reversed order.
 *
 * BUTTERFLY / TWIDDLE RECURRENCE
 * --------------------------------
 * After permutation, log₂(N) passes merge adjacent pairs ("butterflies").
 * At the stage with group length L (half-group L/2), two bins a and b satisfy:
 *
 *   X_new[a] = X[a] + W^k · X[b]
 *   X_new[b] = X[a] - W^k · X[b]
 *
 * where W^k = e^{-j2πk/L} is the twiddle factor for butterfly position k
 * within the group.  W^k is accumulated by complex multiplication from the
 * stage root W^1 = e^{-j2π/L}, avoiding cosf/sinf calls inside the inner
 * loop (only one pair of trig calls per stage, not per butterfly).
 */
static void fft_radix2(float *re, float *im, int n)
{
	/* --- Bit-reversal permutation --- */
	/* j is the bit-reversed index of i; swap (re,im)[i] with (re,im)[j]
	 * whenever i < j to avoid double-swapping. */
	for (int i = 1, j = 0; i < n; i++) {
		int bit = n >> 1; /* start from the most-significant bit position */
		/* Carry-ripple: clear all trailing 1-bits in j until a 0 is found. */
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit; /* set that 0-bit, completing the bit-reversed increment */
		if (i < j) {
			float tr = re[i];
			re[i]    = re[j];
			re[j]    = tr;
			float ti = im[i];
			im[i]    = im[j];
			im[j]    = ti;
		}
	}

	/* --- Butterfly stages --- */
	/* len doubles each stage: 2, 4, 8, … N  (log₂ N stages total). */
	for (int len = 2; len <= n; len <<= 1) {
		/* Primitive twiddle root for this stage: W^1 = e^{-j2π/len}. */
		float ang = -2.0f * (float)M_PI / (float)len;
		float wlr = cosf(ang); /* real part of the stage's unit twiddle root */
		float wli = sinf(ang); /* imag part */

		/* Iterate over each group of 'len' bins in the array. */
		for (int i = 0; i < n; i += len) {
			/* Twiddle accumulator W^k, initialised to W^0 = 1 + 0j. */
			float wr = 1.0f, wi = 0.0f;

			/* Each k processes one butterfly within the group. */
			for (int k = 0; k < len / 2; k++) {
				/* Upper bin a and lower bin b = a + L/2 of this butterfly. */
				int a = i + k;
				int b = i + k + len / 2;
				/* Complex multiply: t = W^k · X[b]. */
				float tr = wr * re[b] - wi * im[b];
				float ti = wr * im[b] + wi * re[b];
				/* In-place DIT butterfly: X[b] = X[a] - t, X[a] = X[a] + t. */
				re[b] = re[a] - tr;
				im[b] = im[a] - ti;
				re[a] += tr;
				im[a] += ti;
				/* Advance twiddle: W^{k+1} = W^k · W^1 (complex multiply). */
				float nwr = wr * wlr - wi * wli;
				wi        = wr * wli + wi * wlr;
				wr        = nwr;
			}
		}
	}
}

/*
 * ase_feat_extract -- compute the full feature vector from one audio frame.
 *
 * Processing pipeline executed in order:
 *
 *   1. DC removal  -- subtract the per-frame mean so the FFT is not swamped
 *      by a large DC bin.  Microphone offsets and low-frequency rumble both
 *      show up as non-zero means that carry no useful acoustic information.
 *
 *   2. Time-domain features: RMS, crest factor, zero-crossing rate.
 *      These are derived directly from the DC-free time signal -- no FFT
 *      is needed -- and capture impulsiveness and HF content cheaply.
 *
 *   3. Radix-2 FFT on the zero-padded DC-free frame → complex spectrum X[k].
 *      Only the single-sided spectrum (bins 1 … N/2-1) is used; the second
 *      half is the conjugate mirror for real input.
 *
 *   4. Single pass over the single-sided spectrum to collect:
 *      spectral centroid, spectral flatness, and the band-energy accumulator.
 *
 *   5. Second pass to compute spectral rolloff (needs total energy first).
 *
 *   6. Normalise band energies by total spectral energy.
 *
 * On entry *out is zeroed; on exit it holds a complete struct ase_features.
 */
void ase_feat_extract(const struct ase_frame_state *st, float sr_hz, struct ase_features *out)
{
	const int n = (st->count < ASE_FRAME_N) ? st->count : ASE_FRAME_N;

	memset(out, 0, sizeof(*out));
	if (n <= 0) {
		return;
	}

	/* --- Step 1: DC removal --- */
	/*
	 * Compute the per-frame mean and subtract it from every sample.
	 * A non-zero mean (microphone bias, low-frequency hum) would otherwise
	 * deposit large energy in bin 0 (DC) and bias the centroid toward zero.
	 * All subsequent calculations operate on the DC-free signal x[i] = s[i] - mean.
	 */
	float mean = 0.0f;
	for (int i = 0; i < n; i++) {
		mean += st->samples[i];
	}
	mean /= (float)n;

	/* --- Step 2: time-domain features (RMS, crest, ZCR) --- */
	/*
	 * Single pass to accumulate the squared-sum (for RMS), the peak magnitude
	 * (for crest factor), and the zero-crossing count (for ZCR):
	 *
	 *   RMS  = sqrt( (1/N) Σ x[i]² )
	 *        Measures average signal power; proportional to perceived loudness.
	 *        Quiet ambient noise is typically RMS < 0.02 (normalised to ±1).
	 *
	 *   crest factor = peak / RMS
	 *        Measures impulsiveness.  A pure 1 kHz sine has crest = √2 ≈ 1.41.
	 *        A transient event (glass break, gunshot, clap) has crest >> 4
	 *        because a brief spike dominates the peak while the RMS stays low.
	 *
	 *   ZCR  = (number of sign changes) / N   ∈ [0, 1]
	 *        A cheap pitch / high-frequency proxy.  A 4 kHz tone at 16 kHz SR
	 *        crosses zero ~0.5 times per sample → ZCR ≈ 0.5.  Voiced speech
	 *        sits at 0.05–0.15; broadband noise near 0.5; near-silence < 0.05.
	 */
	float sum2 = 0.0f, peak = 0.0f;
	int   zc   = 0;
	float prev = st->samples[0] - mean;
	for (int i = 0; i < n; i++) {
		float x = st->samples[i] - mean;
		sum2 += x * x;
		if (fabsf(x) > peak) {
			peak = fabsf(x);
		}
		if (i > 0 && ((x < 0.0f) != (prev < 0.0f))) {
			zc++;
		}
		prev = x;
	}
	out->rms   = sqrtf(sum2 / (float)n);
	out->crest = (out->rms > 1e-6f) ? (peak / out->rms) : 0.0f;
	out->zcr   = (float)zc / (float)n;

	/* --- Step 3: FFT --- */
	/*
	 * Copy the DC-free time signal into the real part of the FFT buffer and
	 * zero-pad the remaining samples.  The imaginary part is zero for real input.
	 * After fft_radix2(), re[k]+j*im[k] = X[k] for k = 0 … ASE_FRAME_N-1.
	 *
	 * We use static buffers here (single-threaded use); the frame owns n ≤ ASE_FRAME_N
	 * real samples.  Bins beyond n are zero-padded, which avoids spectral leakage
	 * from a truncated frame but does not change the frequency resolution.
	 */
	static float re[ASE_FRAME_N];
	static float im[ASE_FRAME_N];
	for (int i = 0; i < ASE_FRAME_N; i++) {
		re[i] = (i < n) ? (st->samples[i] - mean) : 0.0f;
		im[i] = 0.0f;
	}
	fft_radix2(re, im, ASE_FRAME_N);

	/* --- Step 4: single-sided spectrum → centroid, flatness, band energy --- */
	/*
	 * Only bins 1 … N/2-1 carry unique information for real input:
	 *   bin 0 is DC (already removed from the time signal);
	 *   bin N/2 is the real-valued Nyquist bin (treated as an edge case, skipped);
	 *   bins N/2+1 … N-1 are conjugate mirrors of bins N/2-1 … 1.
	 *
	 * Magnitude: |X[k]| = sqrt(re[k]² + im[k]²).  We skip the sqrt for the
	 * rolloff pass (uses |X|²) but keep the magnitude for centroid and flatness.
	 *
	 * Spectral centroid = Σ(f[k] · |X[k]|) / Σ|X[k]|
	 *   The magnitude-weighted mean frequency, often called "spectral brightness".
	 *   High centroid (> 4 kHz) → sibilants, breaking glass, broadband hiss.
	 *   Low centroid (< 1 kHz)  → bass, rumble, voiced speech fundamentals.
	 *
	 * Spectral flatness = geometric_mean(|X|) / arithmetic_mean(|X|)
	 *   Ranges from 0 (perfectly tonal: energy in a single sinusoid) to 1
	 *   (white noise: all bins equally excited).
	 *   Computed as exp(mean(log|X|)) / mean(|X|) to avoid underflow; a tiny
	 *   floor (1e-9) is added before the log to handle near-zero bins.
	 *   A 3150 Hz smoke-detector beep gives flatness ≈ 0.05; rain gives > 0.8.
	 */
	const int half = ASE_FRAME_N / 2;
	float     mag[ASE_FRAME_N / 2];
	float     mag_total = 0.0f, centroid_num = 0.0f, log_sum = 0.0f, lin_sum = 0.0f;
	int       active = 0;
	for (int k = 1; k < half; k++) {
		float m  = sqrtf(re[k] * re[k] + im[k] * im[k]);
		mag[k]   = m;
		float fr = (float)k * sr_hz / (float)ASE_FRAME_N; /* bin centre Hz */
		mag_total += m;
		centroid_num += fr * m; /* accumulate numerator of centroid formula */
		float me = m + 1e-9f;   /* noise floor prevents log(0) */
		log_sum += logf(me);    /* accumulate geometric mean (in log domain) */
		lin_sum += me;          /* accumulate arithmetic mean */
		active++;
	}
	/* Finalise centroid and flatness from the accumulated sums. */
	out->centroid_hz = (mag_total > 1e-12f) ? (centroid_num / mag_total) : 0.0f;
	out->flatness    = (active > 0 && lin_sum > 1e-12f)
	                       ? (expf(log_sum / (float)active) / (lin_sum / (float)active))
	                       : 0.0f;

	/* --- Step 5: spectral rolloff --- */
	/*
	 * Spectral rolloff f_R: the lowest frequency at which the cumulative
	 * spectral energy (using |X|² = power, not magnitude) reaches 85% of total:
	 *
	 *   f_R = min{ f[k] : Σ_{i=1}^{k} |X[i]|² ≥ 0.85 · Σ_{i=1}^{N/2-1} |X[i]|² }
	 *
	 * Using power (|X|²) rather than magnitude here gives a more stable estimate
	 * because it is quadratic in amplitude and less sensitive to a single loud bin.
	 * High rolloff (> 6 kHz) → energy concentrated in HF → glass break or noise.
	 * Low rolloff (< 2 kHz)  → energy concentrated in LF → speech fundamentals.
	 * The 85% threshold is conventional in Music Information Retrieval (MIR).
	 */
	float total2 = 0.0f;
	for (int k = 1; k < half; k++) {
		total2 += mag[k] * mag[k];
	}
	float cum       = 0.0f;
	out->rolloff_hz = 0.0f;
	for (int k = 1; k < half; k++) {
		cum += mag[k] * mag[k];
		if (total2 > 1e-20f && cum >= 0.85f * total2) {
			out->rolloff_hz = (float)k * sr_hz / (float)ASE_FRAME_N;
			break;
		}
	}

	/* --- Step 6: log-spaced band energies --- */
	/*
	 * Map FFT bins to ASE_N_BANDS bands whose boundaries are logarithmically
	 * spaced over bins 1 … N/2-1.  Mapping formula:
	 *
	 *   b = floor( log(k) / log(N/2) · ASE_N_BANDS )   clamped to [0, N_BANDS-1]
	 *
	 * Why logarithmic spacing?  Human hearing resolves frequency on a roughly
	 * logarithmic scale (the mel / bark / ERB scales all share this property).
	 * Equal-width linear bands would give most bins — and most resolution — to
	 * the high-frequency region, where there is often little perceptually useful
	 * content.  Log spacing gives each octave roughly equal representation:
	 *   band 0: sub-200 Hz (room modes, HVAC hum)
	 *   band 3: ~1–2 kHz   (speech body, alarm harmonics)
	 *   band 7: ~4–8 kHz   (sibilance, HF transients, glass break signature)
	 *
	 * Normalisation: each band energy is divided by total2 (total spectral
	 * power) so the 8-element vector sums to 1.  This makes the features
	 * invariant to absolute loudness, capturing the spectral shape only.
	 */
	if (total2 < 1e-20f) {
		return; /* silent frame: leave band_energy[] zeroed */
	}
	for (int k = 1; k < half; k++) {
		float pos = logf((float)k) / logf((float)half); /* 0..1, log-uniform */
		int   b   = (int)(pos * (float)ASE_N_BANDS);
		if (b < 0) {
			b = 0;
		}
		if (b >= ASE_N_BANDS) {
			b = ASE_N_BANDS - 1;
		}
		out->band_energy[b] += mag[k] * mag[k];
	}
	/* Normalise each band by total spectral power. */
	for (int b = 0; b < ASE_N_BANDS; b++) {
		out->band_energy[b] /= total2;
	}
}

/*
 * ase_feat_pack -- serialise the feature struct into a flat float vector.
 *
 * The packing order is fixed; AI model weights are trained against this layout.
 * Changing the order is a breaking model-compatibility change:
 *   [0..7]  band_energy[0..7]  log-spaced tonal fingerprint
 *   [8]     centroid_hz        spectral brightness
 *   [9]     flatness           tonal vs. broadband index
 *   [10]    rolloff_hz         HF energy concentration
 *   [11]    crest              impulsiveness
 *   [12]    zcr                zero-crossing rate
 *   [13]    rms                signal level
 */
size_t ase_feat_pack(const struct ase_features *f, float *vec, size_t cap)
{
	if (cap < (size_t)ASE_FEATURE_DIM) {
		return 0;
	}
	size_t i = 0;
	for (int b = 0; b < ASE_N_BANDS; b++) {
		vec[i++] = f->band_energy[b];
	}
	vec[i++] = f->centroid_hz;
	vec[i++] = f->flatness;
	vec[i++] = f->rolloff_hz;
	vec[i++] = f->crest;
	vec[i++] = f->zcr;
	vec[i++] = f->rms;
	return i; /* == ASE_FEATURE_DIM */
}

/*
 * ase_classify_fallback -- rule-based 4-class acoustic event classifier.
 *
 * Runs when no trained AI model is present.  Thresholds are calibrated for a
 * 16 kHz / 16-bit microphone at typical indoor distances (0.5–3 m).
 * Customers should retrain or retune thresholds for their acoustic environment.
 *
 * BRANCH ORDER IS SIGNIFICANT.
 * A glass-break event has both high crest AND high RMS; if the SCREAM branch
 * (which also keys on high RMS) were tested first, glass breaks would be
 * misclassified.  The ordering below handles this:
 *
 *   1. AMBIENT gate (fast exit) -- cheapest check first, handles silence.
 *   2. GLASS_BREAK  -- impulsive HF burst; must precede the high-RMS SCREAM branch.
 *   3. ALARM        -- narrowband tonal; must precede SCREAM (alarm can have high RMS).
 *   4. SCREAM       -- high-energy voiced harmonic; everything loud that isn't HF tonal.
 *   5. AMBIENT      -- catch-all fallback to minimise false alarms.
 *
 * ACOUSTIC SIGNATURES:
 *
 *   AMBIENT:     rms < 0.02
 *     Background hum, HVAC, silence.  RMS threshold chosen to sit above the
 *     ADC noise floor (~0.001 at 16-bit) while capturing genuine quiet.
 *     Confidence 0.8: low RMS is a reliable silence indicator.
 *
 *   GLASS_BREAK: crest > 4.0  AND  centroid > 4000 Hz  AND  zcr > 0.4
 *     The signature of a breaking window or bottle is a short impulsive burst
 *     of broadband noise biased toward high frequencies.  All three conditions
 *     must hold simultaneously to reduce false positives:
 *       crest > 4   ensures a genuine transient (not sustained loud noise).
 *       centroid > 4 kHz confirms energy is concentrated in the HF region.
 *       zcr > 0.4   confirms broadband HF content (many sign changes per sample).
 *     Confidence 0.85: three independent features make false triggers unlikely.
 *
 *   ALARM:       flatness < 0.2  AND  centroid in [2500, 4000] Hz
 *     Piezoelectric smoke detectors and CO alarms emit a nearly pure tone in
 *     this band (EN 54-3 specifies 3150 ± 500 Hz for fire alarms in Europe).
 *     flatness < 0.2 distinguishes a sinusoidal beep (nearly all energy in one
 *     bin) from broadband noise or speech that happen to have a centroid in range.
 *     Confidence 0.9: the flatness + centroid combination is very discriminative.
 *
 *   SCREAM:      rms > 0.1  AND  centroid in [800, 2500) Hz
 *     Human screaming concentrates energy in the 800 Hz–2.5 kHz range (vocal
 *     tract formants + low harmonics) and reaches sustained high RMS because
 *     screams are not transient.  This branch also captures shrill machinery
 *     alarms or klaxons with harmonics in the voice band that were not caught
 *     by the narrowband ALARM gate.
 *     Confidence 0.75: voice-band energy has more overlap with other classes.
 */
struct ase_verdict ase_classify_fallback(const struct ase_features *f)
{
	/* Default: ambient with medium confidence (overwritten by every branch). */
	struct ase_verdict v = { ASE_AMBIENT, 0.6f };

	if (f->rms < 0.02f) {
		/* Silent / background: very low signal level, almost certainly ambient. */
		v.ev         = ASE_AMBIENT;
		v.confidence = 0.8f;
	} else if (f->crest > 4.0f && f->centroid_hz > 4000.0f && f->zcr > 0.4f) {
		/* Broadband impulsive HF burst: all three discriminators must fire. */
		v.ev         = ASE_GLASS_BREAK;
		v.confidence = 0.85f;
	} else if (f->flatness < 0.2f && f->centroid_hz >= 2500.0f && f->centroid_hz <= 4000.0f) {
		/* Narrowband tonal beep in the EN 54-3 alarm frequency band. */
		v.ev         = ASE_ALARM;
		v.confidence = 0.9f;
	} else if (f->rms > 0.1f && f->centroid_hz >= 800.0f && f->centroid_hz < 2500.0f) {
		/* High-energy voiced harmonic content in the human voice band. */
		v.ev         = ASE_SCREAM;
		v.confidence = 0.75f;
	} else {
		/* No specific pattern matched: report ambient with low confidence. */
		v.ev         = ASE_AMBIENT;
		v.confidence = 0.5f;
	}
	return v;
}

/*
 * ase_event_name -- stable upper-case ASCII label for an event code.
 * Strings are invariant across SDK versions; safe for persistent event logs.
 */
const char *ase_event_name(ase_event_t e)
{
	switch (e) {
	case ASE_AMBIENT:
		return "AMBIENT";
	case ASE_GLASS_BREAK:
		return "GLASS_BREAK";
	case ASE_ALARM:
		return "ALARM";
	case ASE_SCREAM:
		return "SCREAM";
	default:
		return "UNKNOWN";
	}
}
