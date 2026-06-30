/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * current_features implementation -- see current_features.h.
 */

/*
 * Motor Current Signature Analysis (MCSA) -- module overview
 * -----------------------------------------------------------
 * A healthy DC motor draws current with a characteristic commutation ripple
 * whose frequency equals (rotor speed in rev/s) × (commutator segment count).
 * Monitoring this ripple in the current waveform lets firmware distinguish
 * five motor states without any additional speed sensor.
 *
 * Sampling parameters used throughout this file:
 *   CURR_SR_HZ    = 200 Hz  →  Nyquist ceiling at 100 Hz (resolves ripple on
 *                               the low-pole-count motors targeted here).
 *   CURR_WINDOW_N = 256 samples  →  1.28 s window; long enough for several
 *                               commutation cycles at low speed, short enough
 *                               to keep state-update latency under 2 seconds.
 *   FFT bin width = 200 / 256 ≈ 0.78 Hz  →  frequency resolution per bin.
 *
 * Features extracted per window (see struct curr_features in the header):
 *
 *   mean_current_a  DC operating point; primary threshold for OFF / OVERLOAD.
 *   rms_ac_a        RMS of (I − mean): the commutation ripple magnitude.
 *                   KEY MCSA discriminant -- a stalled rotor cannot commutate
 *                   so rms_ac_a collapses even while mean_current_a stays high.
 *   crest           peak|I−mean| / rms_ac; high for spiky transients vs.
 *                   steady periodic ripple.
 *   slope_a         (last-quarter mean) − (first-quarter mean).  Negative
 *                   during decaying inrush; near-zero or positive when running.
 *   mean_power_w    Window-averaged power; corroborates load level.
 *   mean_bus_v      Window-averaged bus voltage; tracks load-induced voltage sag.
 *   ripple_freq_hz  Frequency of the dominant FFT bin; tracks rotor speed.
 */

#include "current_features.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---------------------------------------------------------------------------
 * Window management
 *
 * The window is a simple front-to-back fill buffer (not a circular ring).
 * This design choice keeps the FFT input contiguous in memory (no modular
 * index arithmetic) and makes zero-padding of partial windows trivial.
 * The caller is responsible for calling curr_window_reset() after each
 * successful extraction and before accumulating the next window.
 * -------------------------------------------------------------------------*/

void curr_window_reset(struct curr_window_state *st)
{
	st->count = 0;
}

void curr_window_push(struct curr_window_state *st, struct curr_sample s)
{
	/* Silently discard samples that arrive after the window is full.
	 * The caller should poll curr_window_full() and stop pushing once
	 * true; the silent-drop guard is a safety net against race conditions
	 * in interrupt-driven sample delivery. */
	if (st->count < CURR_WINDOW_N) {
		st->s[st->count++] = s;
	}
}

bool curr_window_full(const struct curr_window_state *st)
{
	return st->count >= CURR_WINDOW_N;
}

/* ---------------------------------------------------------------------------
 * Radix-2 Cooley-Tukey DIT FFT  (in-place, iterative)
 *
 * Algorithm outline
 * -----------------
 * The Cooley-Tukey DIT (decimation-in-time) FFT recursively splits an
 * N-point DFT into two N/2-point DFTs of the even- and odd-indexed inputs,
 * then combines them with "butterfly" operations.  The iterative form avoids
 * stack-costly recursion by working bottom-up over log2(N) stages:
 *
 *   Stage 1 (len=2):  N/2 independent 2-point DFTs.
 *   Stage 2 (len=4):  N/4 independent 4-point DFTs assembled from stage-1.
 *   ...
 *   Stage log2(N):    one N-point DFT assembled from the previous stage.
 *
 * Two preparatory steps:
 *
 *   1. Bit-reversal permutation
 *      -------------------------
 *      The DIT split places even-indexed elements first, which is equivalent
 *      to reading the index in bit-reversed order.  For N=8: index 3 (binary
 *      011) maps to position 110 = 6.  The loop computes successive
 *      bit-reversed indices using a carry-propagation trick (the inner
 *      j-update block), achieving O(1) amortised work per element without a
 *      separate reversal table.  Elements are swapped only when i < j to
 *      avoid double-swapping.
 *
 *   2. Twiddle-factor recurrence
 *      --------------------------
 *      The butterfly for bin k in a stage of length len multiplies the odd
 *      sub-DFT by the twiddle factor W_N^k = exp(−2πi·k/len).  Rather than
 *      calling cosf/sinf inside the innermost loop, the stage root
 *        W_len = cos(−2π/len) + i·sin(−2π/len)
 *      is pre-computed once per stage, then each successive twiddle is
 *      obtained by complex multiplication (the "nwr / wi" update):
 *        W^(k+1) = W^k · W_len
 *      This keeps the hot path free of transcendentals at the cost of small
 *      floating-point drift -- negligible for N = 256.
 *
 * Butterfly formula (combining even element a and twiddle-rotated odd element b):
 *      t        = W^k · X[b]       (complex multiply)
 *      X[b]_new = X[a] − t
 *      X[a]_new = X[a] + t
 *
 * Complexity: O(N log₂ N) -- 2048 butterflies for N=256 vs. 65536 for DFT.
 * N must be a power of 2; CURR_WINDOW_N = 256 satisfies this.
 * -------------------------------------------------------------------------*/
static void fft_radix2(float *re, float *im, int n)
{
	/* --- Phase 1: bit-reversal permutation ---
	 * j is the bit-reversed counterpart of loop index i.  The inner loop
	 * propagates a borrow from the MSB down through j: flip the topmost set
	 * bit (j ^= bit) while it was already set (j & bit), shifting bit right
	 * each time.  Then set the next-lower bit (j ^= bit after the loop).
	 * Only swap when i < j to visit each pair exactly once. */
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

	/* --- Phase 2: butterfly passes, one per stage ---
	 * len doubles each stage (2, 4, 8, …, n).  Each stage merges pairs of
	 * (len/2)-point sub-DFTs that are len/2 apart in the array. */
	for (int len = 2; len <= n; len <<= 1) {
		/* Stage twiddle root W_len = exp(−2πi / len). */
		float ang = -2.0f * (float)M_PI / (float)len;
		float wlr = cosf(ang);
		float wli = sinf(ang);
		for (int i = 0; i < n; i += len) {
			float wr = 1.0f, wi = 0.0f; /* W^0 = 1 + 0i */
			for (int k = 0; k < len / 2; k++) {
				/* Butterfly: a is the even element, b the odd element. */
				int   a  = i + k;
				int   b  = i + k + len / 2;
				float tr = wr * re[b] - wi * im[b]; /* Re(W^k · X[b]) */
				float ti = wr * im[b] + wi * re[b]; /* Im(W^k · X[b]) */
				re[b]    = re[a] - tr;
				im[b]    = im[a] - ti;
				re[a] += tr;
				im[a] += ti;
				/* Advance twiddle one step: W^(k+1) = W^k · W_len. */
				float nwr = wr * wlr - wi * wli;
				wi        = wr * wli + wi * wlr;
				wr        = nwr;
			}
		}
	}
}

/* ---------------------------------------------------------------------------
 * Feature extraction
 * -------------------------------------------------------------------------*/

void curr_feat_extract(const struct curr_window_state *st, float sr_hz, struct curr_features *out)
{
	/* Use however many samples are available; may be < CURR_WINDOW_N if the
	 * window is not yet full (e.g. during the first window after reset). */
	const int n = (st->count < CURR_WINDOW_N) ? st->count : CURR_WINDOW_N;

	/* Zero the output struct unconditionally so all fields have defined
	 * values even in the early-return path (n == 0).  Callers may read
	 * individual fields without first checking if extraction ran. */
	memset(out, 0, sizeof(*out));
	if (n <= 0) {
		return;
	}

	/* --- DC features: mean current, power, bus voltage ---
	 * A single pass accumulates all three sums.  The means serve as the
	 * DC operating-point features.  mean_current_a is the primary classifier
	 * threshold (OFF / OVERLOAD boundary).  mean_bus_v lets the caller
	 * detect load-induced voltage sag independently of the current level. */
	float sum_i = 0.0f, sum_p = 0.0f, sum_v = 0.0f;
	for (int i = 0; i < n; i++) {
		sum_i += st->s[i].current_a;
		sum_p += st->s[i].power_w;
		sum_v += st->s[i].bus_v;
	}
	out->mean_current_a = sum_i / (float)n;
	out->mean_power_w   = sum_p / (float)n;
	out->mean_bus_v     = sum_v / (float)n;

	/* --- AC ripple features: rms_ac_a and crest factor ---
	 * Removing the DC mean before squaring is the software equivalent of AC
	 * coupling: only the commutation ripple energy is measured.
	 *
	 *   rms_ac_a = sqrt( (1/N) · Σ (Iᵢ − mean)² )
	 *
	 * This is the KEY MCSA discriminant.  A stalled rotor produces no
	 * commutation ripple, so rms_ac_a stays near zero while mean_current_a
	 * remains elevated -- the stall/overload fork in current_classify()
	 * exploits exactly this contrast.
	 *
	 * Crest factor = peak_ac / rms_ac.  A high value (>3) indicates that one
	 * or a few large spikes dominate the window rather than a sustained
	 * periodic ripple.  Guard against divide-by-zero with the 1e-6 A floor. */
	float sum2 = 0.0f, peak = 0.0f;
	for (int i = 0; i < n; i++) {
		float ac = st->s[i].current_a - out->mean_current_a;
		sum2 += ac * ac;
		if (fabsf(ac) > peak) {
			peak = fabsf(ac);
		}
	}
	out->rms_ac_a = sqrtf(sum2 / (float)n);
	/* Crest factor is undefined when there is no ripple (e.g. motor OFF or
	 * perfectly smooth supply).  Return 0 rather than +Inf in that case;
	 * the classifier treats 0 and a genuine low crest value identically
	 * because both indicate an absence of peaky transients. */
	out->crest = (out->rms_ac_a > 1e-6f) ? (peak / out->rms_ac_a) : 0.0f;

	/* --- Inrush slope (last-quarter mean − first-quarter mean) ---
	 * During motor start-up, current spikes and then decays as back-EMF
	 * builds.  Averaging the first and last quarters of the window gives a
	 * robust slope estimate that is negative (last < first) while inrush is
	 * decaying and near-zero or positive once running steadily.
	 * q = n/4 with a minimum of 1 to handle tiny partial windows. */
	/* q = number of samples in one quarter.  Minimum of 1 ensures the slope
	 * is computable even when fewer than 4 samples have arrived (the first
	 * ISR tick after a reset), avoiding a divide-by-zero and ensuring the
	 * first and last sums each contain at least one sample. */
	int q = n / 4;
	if (q < 1) {
		q = 1;
	}
	float first = 0.0f, last = 0.0f;
	for (int i = 0; i < q; i++) {
		first += st->s[i].current_a;
		last += st->s[n - 1 - i].current_a;
	}
	/* slope_a is (mean of last quarter) − (mean of first quarter). */
	out->slope_a = (last - first) / (float)q;

	/* --- Dominant ripple frequency via radix-2 FFT ---
	 * Zero-pad the mean-removed current to exactly CURR_WINDOW_N = 256
	 * points (a power of 2) and run the in-place FFT.
	 *
	 * Frequency resolution: Δf = sr_hz / CURR_WINDOW_N = 200/256 ≈ 0.78 Hz.
	 * Usable range: bins 1 to N/2−1 (bin 0 = DC, already removed by mean-
	 * subtraction; bins N/2 and above are aliases).
	 *
	 * We track |X[k]|² (magnitude squared) rather than |X[k]| to avoid a
	 * sqrtf per bin.  Relative ordering is identical so the argmax is the
	 * same.  dom_val starts at −1 so the first bin (k=1) always wins the
	 * first comparison.
	 *
	 * ripple_freq_hz = dom_bin × sr_hz / CURR_WINDOW_N
	 *
	 * The static buffers avoid stack allocation of 2×256×4 = 2 KB on a
	 * constrained Cortex-M55 stack. */
	static float re[CURR_WINDOW_N];
	static float im[CURR_WINDOW_N];
	for (int i = 0; i < CURR_WINDOW_N; i++) {
		re[i] = (i < n) ? (st->s[i].current_a - out->mean_current_a) : 0.0f;
		im[i] = 0.0f;
	}
	fft_radix2(re, im, CURR_WINDOW_N);
	/* Search only the positive-frequency half-spectrum (bins 1 to N/2−1).
	 * Bin 0 (DC) is excluded because the input was already mean-removed.
	 * Bins N/2 and above are mirror images (Nyquist aliases) of bins below. */
	const int half    = CURR_WINDOW_N / 2;
	int       dom_bin = 1;
	float     dom_val = -1.0f;
	for (int k = 1; k < half; k++) {
		float m2 = re[k] * re[k] + im[k] * im[k]; /* |X[k]|² */
		if (m2 > dom_val) {
			dom_val = m2;
			dom_bin = k;
		}
	}
	/* Convert winning bin index to Hz: Δf = sr_hz / N per bin. */
	out->ripple_freq_hz = (float)dom_bin * sr_hz / (float)CURR_WINDOW_N;
}

/* ---------------------------------------------------------------------------
 * Feature vector packing
 *
 * Serialises the seven scalar features into a flat float array in a fixed
 * canonical order that must match the training-time feature ordering used by
 * any downstream ML model.  Returning the count (== CURR_FEATURE_DIM) lets
 * the caller assert at run time that packing succeeded; returning 0 on a
 * too-small buffer flags configuration mismatches early.
 * -------------------------------------------------------------------------*/
size_t curr_feat_pack(const struct curr_features *f, float *vec, size_t cap)
{
	if (cap < (size_t)CURR_FEATURE_DIM) {
		return 0;
	}
	size_t i = 0;
	vec[i++] = f->mean_current_a;
	vec[i++] = f->rms_ac_a;
	vec[i++] = f->crest;
	vec[i++] = f->slope_a;
	vec[i++] = f->mean_power_w;
	vec[i++] = f->mean_bus_v;
	vec[i++] = f->ripple_freq_hz;
	return i; /* == CURR_FEATURE_DIM */
}

/* ---------------------------------------------------------------------------
 * Deterministic five-state classifier
 *
 * Branch order encodes diagnostic priority -- each guard eliminates an
 * ambiguous case before the next test can fire:
 *
 *   1. OFF (mean_current_a < off_a)
 *      Checked first.  A de-energised motor produces noise in every feature;
 *      classifying it as OFF early prevents all subsequent guards from
 *      misfiring on that noise.
 *
 *   2. INRUSH (slope_a < −inrush_slope_a, i.e. current is falling fast)
 *      Checked before the overcurrent guard because startup inrush often
 *      briefly exceeds the overload threshold.  Without this priority the
 *      motor would be misclassified as STALL or OVERLOAD during every cold
 *      start.  A negative slope means last-quarter current < first-quarter
 *      current -- the signature of a decaying startup spike.
 *
 *   3. OVERLOAD vs. STALL (mean_current_a > overload_a)
 *      Both states present with elevated current.  The MCSA ripple
 *      discriminant resolves them:
 *        rms_ac_a < ripple_min_a  →  rotor not turning  →  STALL
 *        rms_ac_a ≥ ripple_min_a  →  rotor still turning →  OVERLOAD
 *      This is the core MCSA insight: a stalled rotor produces no
 *      commutation ripple even though the winding current is high.
 *
 *   4. NORMAL  (fall-through catch-all for everything in-spec)
 * -------------------------------------------------------------------------*/
curr_state_t current_classify(const struct curr_features *f, const struct curr_config *cfg)
{
	if (f->mean_current_a < cfg->off_a) {
		return CURR_OFF;
	}
	if (f->slope_a < -cfg->inrush_slope_a) {
		return CURR_INRUSH; /* current decaying from a startup spike */
	}
	if (f->mean_current_a > cfg->overload_a) {
		/* Ripple discriminant: stall has no commutation ripple, overload does. */
		return (f->rms_ac_a < cfg->ripple_min_a) ? CURR_STALL : CURR_OVERLOAD;
	}
	return CURR_NORMAL;
}

/* curr_state_name -- string table for logging and telemetry.
 * Returns a stable upper-case ASCII identifier; callers may log the pointer
 * directly without copying.  The default branch catches any value outside the
 * defined enum range (e.g. an uninitialised variable). */
const char *curr_state_name(curr_state_t s)
{
	switch (s) {
	case CURR_OFF:
		return "OFF";
	case CURR_NORMAL:
		return "NORMAL";
	case CURR_INRUSH:
		return "INRUSH";
	case CURR_OVERLOAD:
		return "OVERLOAD";
	case CURR_STALL:
		return "STALL";
	default:
		return "UNKNOWN";
	}
}

/* ---------------------------------------------------------------------------
 * Deterministic anomaly fallback  (no AI model loaded)
 *
 * Returns a 0..1 severity score for overcurrent conditions.  This is the
 * fallback path when the AI inference engine is unavailable (model not yet
 * loaded, NPU offline, first-boot before OTA delivers the model file).
 *
 * Scoring:
 *   Proportional overcurrent:
 *     score = (I_mean − I_overload) / I_overload
 *     Gives 0.0 exactly at the overload threshold, rising linearly with
 *     excess current.  The cfg->overload_a > 1e-6 guard prevents divide-
 *     by-zero when the config is uninitialised.
 *
 *   Stall guard:
 *     If high current AND near-zero ripple (rms_ac_a < ripple_min_a), the
 *     rotor has stopped.  A stalled winding dissipates full supply power
 *     with no mechanical output -- the most thermally destructive common
 *     fault.  The score is saturated to 0.9 regardless of the proportional
 *     value so that a stall always triggers the highest-priority alert tier.
 *
 *   Hard clamp to [0.0, 1.0] for a normalised downstream interface.
 * -------------------------------------------------------------------------*/
float curr_anomaly_fallback(const struct curr_features *f, const struct curr_config *cfg)
{
	float score = 0.0f;
	if (f->mean_current_a > cfg->overload_a && cfg->overload_a > 1e-6f) {
		/* Fractional excess: 0 at threshold, grows linearly above it. */
		score = (f->mean_current_a - cfg->overload_a) / cfg->overload_a;
	}
	/* High current with no ripple = stalled rotor: a strong anomaly. */
	if (f->mean_current_a > cfg->overload_a && f->rms_ac_a < cfg->ripple_min_a) {
		score = fmaxf(score, 0.9f);
	}
	/* Clamp to [0, 1].  The proportional formula can only produce ≥ 0, but
	 * the clamps guard against future extensions that might compute a
	 * negative score and ensure a normalised output contract regardless. */
	if (score < 0.0f) {
		score = 0.0f;
	}
	if (score > 1.0f) {
		score = 1.0f;
	}
	return score;
}
