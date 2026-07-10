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

/*
 * SPECTRAL + STATISTICAL MATH -- library-backed, not hand-rolled.
 * ---------------------------------------------------------------
 * This example deliberately does NOT re-derive an FFT or the window
 * statistics by hand.  Two library surfaces do the numeric work, and the
 * SAME source builds for native_sim and the Cortex-M55 alike:
 *
 *   1. The dominant-ripple FFT goes through the portable <alp/dsp.h> chain
 *      API.  Its backend runs ARM CMSIS-DSP (arm_rfft_fast_f32, Helium-
 *      vectorised) on the M55 and a portable-C radix-2 fallback under
 *      native_sim -- selected by the backend registry, not an #ifdef here.
 *      We call the alp wrapper (not arm_rfft_* directly) because alp-sdk
 *      SHIPS a portable FFT surface: keeping the example on it lets the
 *      identical source run on the V2N (A55 + DRP-AI) path too.
 *
 *   2. The scalar window statistics (per-channel mean, RMS, abs-peak) call
 *      ARM CMSIS-DSP arm_*_f32 kernels DIRECTLY -- alp-sdk has no portable
 *      scalar-stats surface to wrap, and these kernels are the idiomatic
 *      accelerated path on Cortex-M.  They are guarded by __has_include
 *      with a portable-C fallback so the native_sim gate still builds.
 *
 * CMSIS kernels operate on a single contiguous float array.  The INA236
 * sample stream is an array-of-structs (current_a/bus_v/power_w
 * interleaved per sample), so each channel is de-interleaved into its own
 * contiguous buffer once per window before the kernels run -- see the
 * de-interleave loop in curr_feat_extract, which stays a plain portable
 * loop (there is no CMSIS "strided gather" kernel to replace it with).
 */
#if defined(__has_include)
#if __has_include("arm_math.h")
#include "arm_math.h"
#define CURR_HAS_CMSIS_DSP 1
#endif
#endif
#ifndef CURR_HAS_CMSIS_DSP
#define CURR_HAS_CMSIS_DSP 0
#endif

#include <alp/dsp.h>

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

	/* De-interleave the AoS sample stream into three contiguous per-channel
	 * buffers.  This pass is NOT a "stat" itself -- it is unavoidable data
	 * staging: every CMSIS-DSP kernel below (arm_mean_f32, arm_offset_f32,
	 * arm_rms_f32, arm_absmax_f32) requires a plain contiguous float array,
	 * and the INA236 sample struct interleaves current/bus/power per
	 * sample.  One combined pass here is cheaper than three separate
	 * per-channel copy passes.  cur_buf is reused below for the AC-ripple
	 * stats and the FFT input; it is static to keep the 3 * 256 * 4 = 3 KB
	 * of buffers off the stack. */
	static float cur_buf[CURR_WINDOW_N];
	static float pwr_buf[CURR_WINDOW_N];
	static float bus_buf[CURR_WINDOW_N];
	for (int i = 0; i < n; i++) {
		cur_buf[i] = st->s[i].current_a;
		pwr_buf[i] = st->s[i].power_w;
		bus_buf[i] = st->s[i].bus_v;
	}

	/* --- DC features: mean current, power, bus voltage ---
	 * The means serve as the DC operating-point features.  mean_current_a
	 * is the primary classifier threshold (OFF / OVERLOAD boundary).
	 * mean_bus_v lets the caller detect load-induced voltage sag
	 * independently of the current level. */
#if CURR_HAS_CMSIS_DSP
	arm_mean_f32(cur_buf, (uint32_t)n, &out->mean_current_a);
	arm_mean_f32(pwr_buf, (uint32_t)n, &out->mean_power_w);
	arm_mean_f32(bus_buf, (uint32_t)n, &out->mean_bus_v);
#else
	/* Portable-C fallback (native_sim, or any target without CMSIS-DSP);
	 * reproduces the same sum-then-divide arithmetic as arm_mean_f32. */
	float sum_i = 0.0f, sum_p = 0.0f, sum_v = 0.0f;
	for (int i = 0; i < n; i++) {
		sum_i += cur_buf[i];
		sum_p += pwr_buf[i];
		sum_v += bus_buf[i];
	}
	out->mean_current_a = sum_i / (float)n;
	out->mean_power_w   = sum_p / (float)n;
	out->mean_bus_v     = sum_v / (float)n;
#endif

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
	 * periodic ripple.  Guard against divide-by-zero with the 1e-6 A floor.
	 *
	 * xc[] (the mean-centred current) is reused below as the FFT input, so
	 * it is computed here once via arm_offset_f32 rather than recomputed
	 * per stage. */
	static float xc[CURR_WINDOW_N];
	float        peak;
#if CURR_HAS_CMSIS_DSP
	uint32_t peak_idx;
	arm_offset_f32(cur_buf, -out->mean_current_a, xc, (uint32_t)n);
	arm_rms_f32(xc, (uint32_t)n, &out->rms_ac_a); /* rms = sqrt(mean(xc^2)) */
	arm_absmax_f32(xc, (uint32_t)n, &peak, &peak_idx);
#else
	float sum2 = 0.0f;
	peak       = 0.0f;
	for (int i = 0; i < n; i++) {
		xc[i]    = cur_buf[i] - out->mean_current_a;
		float ax = fabsf(xc[i]);
		sum2 += xc[i] * xc[i];
		if (ax > peak) {
			peak = ax;
		}
	}
	out->rms_ac_a = sqrtf(sum2 / (float)n);
#endif
	/* Crest factor is undefined when there is no ripple (e.g. motor OFF or
	 * perfectly smooth supply).  Return 0 rather than +Inf in that case;
	 * the classifier treats 0 and a genuine low crest value identically
	 * because both indicate an absence of peaky transients. */
	out->crest = (out->rms_ac_a > 1e-6f) ? (peak / out->rms_ac_a) : 0.0f;
	for (int i = n; i < CURR_WINDOW_N; i++) {
		xc[i] = 0.0f; /* zero-pad the FFT tail on a short window */
	}

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
#if CURR_HAS_CMSIS_DSP
	/* mean-of-first-q and mean-of-last-q via arm_mean_f32 over a sub-range
	 * of cur_buf; algebraically identical to (last_sum - first_sum) / q. */
	float first_mean, last_mean;
	arm_mean_f32(cur_buf, (uint32_t)q, &first_mean);
	arm_mean_f32(&cur_buf[n - q], (uint32_t)q, &last_mean);
	out->slope_a = last_mean - first_mean;
#else
	float first = 0.0f, last = 0.0f;
	for (int i = 0; i < q; i++) {
		first += cur_buf[i];
		last += cur_buf[n - 1 - i];
	}
	/* slope_a is (mean of last quarter) − (mean of first quarter). */
	out->slope_a = (last - first) / (float)q;
#endif

	/*
	 * SPECTRUM via the portable <alp/dsp.h> chain (NOT a hand-rolled FFT).
	 * ------------------------------------------------------------------
	 * A single ALP_DSP_STAGE_FFT (rectangular, no window) transforms the
	 * mean-centred current xc[] to magnitude bins.  The backend runs
	 * CMSIS-DSP arm_rfft_fast_f32 on the M55 and a portable-C radix-2 FFT
	 * under native_sim -- the example source is identical either way.
	 *
	 * The chain consumes int16 samples (the wire format used elsewhere in
	 * alp-sdk for ADC-derived streams).  We scale the float window to fill
	 * the int16 range before feeding it: the absolute scale is irrelevant
	 * here because the dominant bin is an argmax over magnitude-squared,
	 * which is scale-invariant.
	 */
	static int16_t samp_q15[CURR_WINDOW_N];
	float          scale = (peak > 1e-9f) ? (30000.0f / peak) : 0.0f;
	for (int i = 0; i < CURR_WINDOW_N; i++) {
		samp_q15[i] = (int16_t)lrintf(xc[i] * scale);
	}

	static float    mag[CURR_WINDOW_N];
	alp_dsp_stage_t stages[] = {
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points = CURR_WINDOW_N, .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE } },
	};
	alp_dsp_chain_t *chain = alp_dsp_chain_open(stages, 1u);
	size_t           got   = 0;
	memset(mag, 0, sizeof(mag));
	if (chain != NULL) {
		(void)alp_dsp_chain_apply_bins(chain, samp_q15, CURR_WINDOW_N, mag, CURR_WINDOW_N, &got);
		alp_dsp_chain_close(chain);
	}

	/* Search only the positive-frequency half-spectrum (bins 1 to N/2−1).
	 * Bin 0 (DC) is excluded because the input was already mean-removed.
	 * Bins N/2 and above are mirror images (Nyquist aliases) of bins below.
	 * We track |X[k]|² (magnitude squared, from the chain's magnitude
	 * output) rather than |X[k]| to preserve the original argmax ordering.
	 * dom_val starts at −1 so the first bin (k=1) always wins the first
	 * comparison. */
	const int half    = CURR_WINDOW_N / 2;
	int       dom_bin = 1;
	float     dom_val = -1.0f;
	for (int k = 1; k < half; k++) {
		float m2 = mag[k] * mag[k]; /* |X[k]|² */
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
