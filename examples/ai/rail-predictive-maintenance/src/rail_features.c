/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rail_features implementation -- see rail_features.h.
 */
#include "rail_features.h"

#include <math.h>
#include <string.h>

/*
 * SPECTRAL + STATISTICAL MATH -- library-backed, not hand-rolled.
 * ---------------------------------------------------------------
 * This example deliberately does NOT re-derive an FFT or the moment
 * statistics by hand.  Two library surfaces do the numeric work, and
 * the SAME source builds for native_sim and the Cortex-M55 alike:
 *
 *   1. The FFT goes through the portable <alp/dsp.h> chain API.  Its
 *      backend runs ARM CMSIS-DSP (arm_rfft_fast_f32, Helium-vectorised)
 *      on the M55, a hardware DSP block where the SoM has one, and a
 *      portable-C radix-2 fallback under native_sim -- selected by the
 *      backend registry, not an #ifdef here.  We call the alp wrapper
 *      (not arm_rfft_* directly) because alp-sdk SHIPS a portable FFT
 *      surface: keeping the example on it lets the identical source run
 *      on the V2N (A55 + DRP-AI) and NXP paths, which have no CMSIS-M.
 *
 *   2. The scalar window statistics (mean, RMS, abs-peak) call ARM
 *      CMSIS-DSP arm_*_f32 kernels DIRECTLY -- alp-sdk has no portable
 *      scalar-stats surface to wrap, and these kernels are the idiomatic
 *      accelerated path on Cortex-M.  They are guarded by __has_include
 *      with a portable-C fallback so the native_sim gate still builds.
 */
#if defined(__has_include)
#if __has_include("arm_math.h")
#include "arm_math.h"
#define RAIL_HAS_CMSIS_DSP 1
#endif
#endif
#ifndef RAIL_HAS_CMSIS_DSP
#define RAIL_HAS_CMSIS_DSP 0
#endif

#include <alp/dsp.h>

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

	/*
	 * MEAN-CENTRE THE WINDOW (shared by the stats AND the FFT).
	 *
	 * xc[i] = sample[i] - mean is the AC (vibration) component with the
	 * DC bias (gravity, sensor offset) removed.  The tail [n, N) is
	 * zero-padded so the fixed-size FFT can run on a short window;
	 * zero-padding adds no spectral energy, it only interpolates the
	 * spectrum to finer bin spacing.  xc is static (single sensor
	 * thread, no re-entrancy) to keep a 1 kB buffer off the stack.
	 */
	static float xc[RAIL_WINDOW_N];
	float        peak;
	float        var;

#if RAIL_HAS_CMSIS_DSP
	/*
	 * CMSIS-DSP path (Cortex-M55: Helium-vectorised kernels).
	 * arm_mean_f32 / arm_offset_f32 / arm_rms_f32 / arm_absmax_f32 are
	 * the idiomatic accelerated primitives; alp-sdk has no portable
	 * scalar-stats surface to wrap, so we call them directly.
	 */
	float mean;
	arm_mean_f32(st->samples, (uint32_t)n, &mean);
	arm_offset_f32(st->samples, -mean, xc, (uint32_t)n);

	uint32_t peak_idx;
	arm_rms_f32(xc, (uint32_t)n, &out->rms); /* rms = sqrt(mean(xc^2))    */
	arm_absmax_f32(xc, (uint32_t)n, &peak, &peak_idx);
	var = out->rms * out->rms; /* mean(xc)=0 => var = rms^2  */
#else
	/* Portable-C fallback (native_sim, or any target without CMSIS-DSP). */
	float mean = 0.0f;
	for (int i = 0; i < n; i++) {
		mean += st->samples[i];
	}
	mean /= (float)n;

	float sum2 = 0.0f;
	peak       = 0.0f;
	for (int i = 0; i < n; i++) {
		xc[i]    = st->samples[i] - mean;
		float ax = fabsf(xc[i]);
		sum2 += xc[i] * xc[i];
		if (ax > peak) {
			peak = ax;
		}
	}
	var      = sum2 / (float)n;
	out->rms = sqrtf(var);
#endif
	for (int i = n; i < RAIL_WINDOW_N; i++) {
		xc[i] = 0.0f; /* zero-pad the FFT tail on a short window */
	}

	/*
	 * TIME-DOMAIN FEATURES derived from the centred window:
	 *
	 *   Crest factor (peak/RMS) -- for band-limited Gaussian noise
	 *     CF ~= 3-4; a single-cycle impulse (wheel flat, joint impact)
	 *     drives CF to 10-20.  Guard rms < 1e-9 clamps CF to 0.
	 *   Kurtosis (K = E[x^4]/E[x^2]^2) -- the 4th standardised moment;
	 *     Gaussian noise gives K ~= 3, impulsive events push K past 5.
	 *     CMSIS-DSP ships no 4th-moment kernel, so the sum-of-4th-powers
	 *     stays a portable loop over xc.  Guard var < 1e-12 clamps K to 0.
	 */
	out->crest_factor = (out->rms > 1e-9f) ? (peak / out->rms) : 0.0f;
	float sum4        = 0.0f;
	for (int i = 0; i < n; i++) {
		float x2 = xc[i] * xc[i];
		sum4 += x2 * x2;
	}
	out->kurtosis = (var > 1e-12f) ? ((sum4 / (float)n) / (var * var)) : 0.0f;

	/*
	 * SPECTRUM via the portable <alp/dsp.h> chain (NOT a hand-rolled FFT).
	 * ------------------------------------------------------------------
	 * A single ALP_DSP_STAGE_FFT (rectangular, no window) transforms the
	 * centred window to magnitude bins.  The backend runs CMSIS-DSP
	 * arm_rfft_fast_f32 on the M55 and a portable-C radix-2 FFT under
	 * native_sim -- the example source is identical either way.
	 *
	 * The chain consumes int16 samples (the accelerometer's native ADC
	 * format).  We scale the float window to fill the int16 range before
	 * feeding it: the absolute scale is irrelevant here because every
	 * downstream spectral feature -- the dominant bin (an argmax) and the
	 * normalised band energies -- is scale-invariant.
	 */
	static int16_t samp_q15[RAIL_WINDOW_N];
	float          scale = (peak > 1e-9f) ? (30000.0f / peak) : 0.0f;
	for (int i = 0; i < RAIL_WINDOW_N; i++) {
		samp_q15[i] = (int16_t)lrintf(xc[i] * scale);
	}

	static float    mag[RAIL_WINDOW_N];
	alp_dsp_stage_t stages[] = {
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points = RAIL_WINDOW_N, .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE } },
	};
	alp_dsp_chain_t *chain = alp_dsp_chain_open(stages, 1u);
	size_t           got   = 0;
	memset(mag, 0, sizeof(mag));
	if (chain != NULL) {
		(void)alp_dsp_chain_apply_bins(chain, samp_q15, RAIL_WINDOW_N, mag, RAIL_WINDOW_N, &got);
		alp_dsp_chain_close(chain);
	}

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
		mag2[k] = mag[k] * mag[k];         /* magnitude-squared (power) per bin */
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
