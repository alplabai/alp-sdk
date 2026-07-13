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

/*
 * SPECTRAL + STATISTICAL MATH -- via <alp/dsp.h>, not hand-rolled.
 * ---------------------------------------------------------------
 * This file does NOT re-derive an FFT or the window statistics by hand,
 * and it does NOT call CMSIS-DSP arm_* directly.  Both go through the
 * portable <alp/dsp.h> surface, so the SAME source builds for native_sim
 * and the Cortex-M55 alike and the SDK -- not this file -- owns the
 * CMSIS-vs-portable choice:
 *
 *   1. FFT -> alp_dsp_chain (ALP_DSP_STAGE_FFT).  The backend runs
 *      CMSIS-DSP arm_rfft_fast_f32 (Helium-vectorised) on the M55, a
 *      hardware DSP block where the SoM has one, and a portable-C radix-2
 *      fallback under native_sim -- selected by the backend registry.
 *      Keeping this on <alp/dsp> lets the identical source run on the
 *      V2N (A55 + DRP-AI) and NXP paths, which have no CMSIS-M.
 *
 *   2. Time-domain window statistics (mean, RMS, abs-peak) ->
 *      alp_dsp_stats_f32, which the SDK backs with CMSIS-DSP
 *      arm_mean/rms/absmax_f32 on Cortex-M and a portable-C pass
 *      elsewhere.
 *
 * Only the zero-crossing rate stays a hand-written loop -- it has no
 * CMSIS (or alp_dsp) kernel.
 */
#include <alp/dsp.h>

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
 *   3. <alp/dsp.h> FFT chain on the zero-padded DC-free frame -> magnitude
 *      spectrum.  Only the single-sided spectrum (bins 1 ... N/2-1) is used;
 *      the second half is the conjugate mirror for real input.
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

	/*
	 * DC-REMOVED WINDOW (shared by the time-domain stats AND the FFT).
	 *
	 * xc[i] = sample[i] - mean is the AC (acoustic) component with the
	 * DC bias (mic offset, low-frequency rumble) removed.  The tail
	 * [n, ASE_FRAME_N) is zero-padded so the fixed-size FFT can run on a
	 * short frame; zero-padding adds no spectral energy, it only
	 * interpolates the spectrum to finer bin spacing.  xc is static
	 * (single audio thread, no re-entrancy) to keep a 2 kB buffer off
	 * the stack.
	 *
	 * One alp_dsp_stats_f32 pass over the raw frame yields the mean; a
	 * second over the mean-centred xc yields the RMS and abs-peak with
	 * the DC bias already removed.  The SDK backs these with CMSIS-DSP
	 * arm_mean/rms/absmax_f32 on the M55 and a portable-C pass under
	 * native_sim -- no arm_* here.
	 */
	static float xc[ASE_FRAME_N];

	alp_dsp_stats_t raw;
	alp_dsp_stats_f32(st->samples, (size_t)n, &raw);
	const float mean = raw.mean;

	for (int i = 0; i < n; i++) {
		xc[i] = st->samples[i] - mean;
	}
	for (int i = n; i < ASE_FRAME_N; i++) {
		xc[i] = 0.0f; /* zero-pad the FFT tail on a short frame */
	}

	alp_dsp_stats_t ac;
	alp_dsp_stats_f32(xc, (size_t)n, &ac);
	const float peak = ac.abs_max; /* peak |xc|              */
	out->rms         = ac.rms;     /* RMS = sqrt(mean(xc^2)) */

	/*
	 * CREST FACTOR: peak / RMS, from the DC-free window computed above.
	 * A pure sine has crest = sqrt(2) ~= 1.41; a transient event (glass
	 * break, gunshot, clap) has crest >> 4 because a brief spike
	 * dominates the peak while the RMS stays low.
	 */
	out->crest = (out->rms > 1e-6f) ? (peak / out->rms) : 0.0f;

	/*
	 * ZERO-CROSSING RATE -- a cheap pitch / high-frequency proxy.
	 * CMSIS-DSP (and alp_dsp) ship no zero-crossing kernel, so this
	 * stays a portable loop over xc.
	 *
	 *   ZCR = (number of sign changes) / N   in [0, 1]
	 *     A 4 kHz tone at 16 kHz SR crosses zero ~0.5 times per sample ->
	 *     ZCR ~= 0.5.  Voiced speech sits at 0.05-0.15; broadband noise
	 *     near 0.5; near-silence < 0.05.
	 */
	int   zc   = 0;
	float prev = xc[0];
	for (int i = 1; i < n; i++) {
		if ((xc[i] < 0.0f) != (prev < 0.0f)) {
			zc++;
		}
		prev = xc[i];
	}
	out->zcr = (float)zc / (float)n;

	/*
	 * SPECTRUM via the portable <alp/dsp.h> chain (NOT a hand-rolled FFT).
	 * ------------------------------------------------------------------
	 * A single ALP_DSP_STAGE_FFT (rectangular, no window) transforms the
	 * DC-free frame to magnitude bins.  The backend runs CMSIS-DSP
	 * arm_rfft_fast_f32 on the M55 and a portable-C radix-2 FFT under
	 * native_sim -- this file's source is identical either way.
	 *
	 * The chain consumes int16 samples (the microphone's native PCM
	 * format).  We scale the float frame to fill the int16 range before
	 * feeding it: the absolute scale is irrelevant here because every
	 * downstream spectral feature -- centroid (a ratio), flatness (a
	 * ratio), rolloff and the normalised band energies -- is
	 * scale-invariant.
	 */
	static int16_t samp_q15[ASE_FRAME_N];
	float          scale = (peak > 1e-9f) ? (30000.0f / peak) : 0.0f;
	for (int i = 0; i < ASE_FRAME_N; i++) {
		samp_q15[i] = (int16_t)lrintf(xc[i] * scale);
	}

	static float    mag[ASE_FRAME_N];
	alp_dsp_stage_t stages[] = {
		{ .kind  = ALP_DSP_STAGE_FFT,
		  .u.fft = { .n_points = ASE_FRAME_N, .output_format = ALP_DSP_FFT_OUTPUT_MAGNITUDE } },
	};
	alp_dsp_chain_t *chain = alp_dsp_chain_open(stages, 1u);
	size_t           got   = 0;
	memset(mag, 0, sizeof(mag));
	if (chain != NULL) {
		(void)alp_dsp_chain_apply_bins(chain, samp_q15, ASE_FRAME_N, mag, ASE_FRAME_N, &got);
		alp_dsp_chain_close(chain);
	}

	/*
	 * SINGLE-SIDED SPECTRUM -> centroid, flatness, band energy.
	 * -----------------------------------------------------------
	 * Only bins 1 ... N/2-1 carry unique information for real input:
	 *   bin 0 is DC (already removed from the time signal);
	 *   bin N/2 is the real-valued Nyquist bin (treated as an edge case,
	 *   skipped);
	 *   bins N/2+1 ... N-1 are conjugate mirrors of bins N/2-1 ... 1.
	 *
	 * Spectral centroid = Sum(f[k] * |X[k]|) / Sum|X[k]|
	 *   The magnitude-weighted mean frequency, often called "spectral
	 *   brightness".  High centroid (> 4 kHz) -> sibilants, breaking
	 *   glass, broadband hiss.  Low centroid (< 1 kHz) -> bass, rumble,
	 *   voiced speech fundamentals.  A ratio of magnitudes, so the
	 *   uncalibrated int16 scale of the chain's output cancels out.
	 *
	 * Spectral flatness = geometric_mean(|X|) / arithmetic_mean(|X|)
	 *   Ranges from 0 (perfectly tonal: energy in a single sinusoid) to 1
	 *   (white noise: all bins equally excited).
	 *   Computed as exp(mean(log|X|)) / mean(|X|) to avoid underflow; a
	 *   tiny floor (1e-9) is added before the log to handle near-zero
	 *   bins -- negligible next to the chain's int16-scale magnitudes.
	 *   A 3150 Hz smoke-detector beep gives flatness ~= 0.05; rain gives
	 *   > 0.8.
	 */
	const int half      = ASE_FRAME_N / 2;
	float     mag_total = 0.0f, centroid_num = 0.0f, log_sum = 0.0f, lin_sum = 0.0f;
	int       active = 0;
	for (int k = 1; k < half; k++) {
		float m  = mag[k];
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

	/*
	 * SPECTRAL ROLLOFF
	 * ----------------
	 * Spectral rolloff f_R: the lowest frequency at which the cumulative
	 * spectral energy (using |X|^2 = power, not magnitude) reaches 85% of
	 * total:
	 *
	 *   f_R = min{ f[k] : Sum_{i=1}^{k} |X[i]|^2 >= 0.85 * Sum_{i=1}^{N/2-1} |X[i]|^2 }
	 *
	 * Using power (|X|^2) rather than magnitude here gives a more stable
	 * estimate because it is quadratic in amplitude and less sensitive to
	 * a single loud bin -- and, like centroid/flatness above, the ratio
	 * against the total is invariant to the chain's absolute int16 scale.
	 * High rolloff (> 6 kHz) -> energy concentrated in HF -> glass break
	 * or noise.  Low rolloff (< 2 kHz) -> energy concentrated in LF ->
	 * speech fundamentals.  The 85% threshold is conventional in Music
	 * Information Retrieval (MIR).
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

	/*
	 * LOG-SPACED BAND ENERGIES
	 * -------------------------
	 * Map FFT bins to ASE_N_BANDS bands whose boundaries are
	 * logarithmically spaced over bins 1 ... N/2-1.  Mapping formula:
	 *
	 *   b = floor( log(k) / log(N/2) * ASE_N_BANDS )   clamped to [0, N_BANDS-1]
	 *
	 * Why logarithmic spacing?  Human hearing resolves frequency on a
	 * roughly logarithmic scale (the mel / bark / ERB scales all share
	 * this property).  Equal-width linear bands would give most bins --
	 * and most resolution -- to the high-frequency region, where there is
	 * often little perceptually useful content.  Log spacing gives each
	 * octave roughly equal representation.
	 *
	 * Band boundaries follow k = 2^b for band b (b = floor(log(k)/log(256)*8)),
	 * where bin->Hz = k * 31.25 (= 16000/512).  Approximate band centres:
	 *   band 0: ~31-62 Hz   (sub-bass, room modes, HVAC hum)
	 *   band 3: ~250-500 Hz (upper bass, low-speech fundamentals)
	 *   band 7: ~4-8 kHz    (sibilance, HF transients, glass break signature)
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
