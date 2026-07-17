/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * acoustic_event -- pure-C per-frame DSP feature extraction for the acoustic
 * safety-event classifier.  Arch-neutral: the spectral and window-statistics
 * maths go through the portable <alp/dsp.h> surface rather than CMSIS arm_*
 * directly, so the same source builds for native_sim and the Cortex-M55
 * alike; host-unit-tested.
 *
 * DESIGN OVERVIEW
 * ---------------
 * The processing chain for each audio frame is:
 *
 *   microphone samples ──► ase_frame_push()
 *                         (accumulate ASE_FRAME_N samples)
 *                ▼
 *   ase_feat_extract()
 *    ├─ DC removal + time-domain: RMS, crest factor, zero-crossing rate
 *    ├─ <alp/dsp.h> FFT chain → single-sided magnitude spectrum
 *    ├─ Spectral centroid, flatness, rolloff
 *    └─ Log-spaced band energies (ASE_N_BANDS bands, normalised to sum 1)
 *                ▼
 *   ase_feat_pack()  →  float[ASE_FEATURE_DIM]   (model input vector)
 *                ▼
 *   ase_classify_fallback()  or  AI model inference
 *                ▼
 *   struct ase_verdict { event, confidence }
 *
 * All state is caller-owned (struct ase_frame_state, struct ase_features).
 * There are no global mutable data structures visible to the application.
 * The static FFT scratch buffers inside ase_feat_extract are not re-entrant
 * but are safe for single-threaded or cooperative Zephyr task use.
 */
#ifndef ACOUSTIC_EVENT_H
#define ACOUSTIC_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Frame and feature-vector parameters.
 *
 * ASE_FRAME_N     512 samples at 16 kHz = 32 ms per frame.  A 32 ms window
 *                 gives ~31 Hz per FFT bin (16000/512), which is enough to
 *                 separate a 3150 Hz smoke-detector beep from adjacent noise.
 *                 Must be a power of two for the FFT stage.
 *
 * ASE_SR_HZ       Expected input sample rate.  Passed explicitly to
 *                 ase_feat_extract() so unit tests can use any rate.
 *
 * ASE_N_BANDS     Number of log-spaced energy bands.  8 bands spans roughly
 *                 one octave per band from ~30 Hz to 8 kHz (Nyquist at 16 kHz).
 *
 * ASE_FEATURE_DIM Total floats in the packed feature vector:
 *                   8 band energies + centroid + flatness + rolloff +
 *                   crest + zcr + rms  =  14 elements.
 */
#define ASE_FRAME_N 512
#define ASE_SR_HZ   16000.0f
#define ASE_N_BANDS 8
/** 8 band energies + centroid + flatness + rolloff + crest + zcr + rms. */
#define ASE_FEATURE_DIM 14

/**
 * Per-frame sample accumulator.
 *
 * Fill via ase_frame_push() until ase_frame_full() returns true, then call
 * ase_feat_extract().  Reset with ase_frame_reset() before the next frame.
 * Caller owns storage; no heap allocation takes place.
 */
struct ase_frame_state {
	float    samples[ASE_FRAME_N]; /**< raw (pre-DC-removal) samples, newest last */
	uint16_t count;                /**< number of valid samples pushed so far */
};

/**
 * Extracted feature vector for one analysis frame.
 *
 * Produced by ase_feat_extract(); optionally serialised by ase_feat_pack().
 * Features are grouped by computation stage:
 *
 *   Time-domain (DC-free):  rms, crest, zcr
 *   Frequency-domain:       band_energy[], centroid_hz, flatness, rolloff_hz
 *
 * All values are non-negative.  rms, crest, zcr, and flatness are
 * dimensionless; centroid_hz and rolloff_hz are in Hz (0 … sr_hz/2).
 */
struct ase_features {
	float band_energy[ASE_N_BANDS]; /**< normalised log-band energy (sums to 1). */
	float centroid_hz;              /**< spectral brightness: Σ(f·|X|)/Σ|X|, Hz. */
	float flatness;                 /**< geo/arith magnitude mean: ~1 broadband, ~0 tonal. */
	float rolloff_hz;               /**< freq below which 85% of spectral energy lies. */
	float crest;                    /**< peak/RMS impulsiveness; pure sine ≈ 1.41. */
	float zcr;                      /**< zero-crossing rate, [0..1] per sample. */
	float rms;                      /**< AC RMS after DC removal. */
};

/** Reset the frame accumulator; call before the very first push. */
void ase_frame_reset(struct ase_frame_state *st);

/**
 * Push one sample into the frame accumulator.
 *
 * Samples beyond ASE_FRAME_N are silently dropped.  Check ase_frame_full()
 * before pushing if sample loss must be detected.
 */
void ase_frame_push(struct ase_frame_state *st, float sample);

/** Return true once ASE_FRAME_N samples have been accumulated. */
bool ase_frame_full(const struct ase_frame_state *st);

/**
 * Extract the full feature vector from a completed (or partial) frame.
 *
 * @param st     Frame accumulator; ase_frame_full() should be true for a
 *               full 32 ms frame.  Partial frames are handled (fewer bins).
 * @param sr_hz  Sample rate of the audio held in *st, in Hz (ASE_SR_HZ
 *               for production use; any rate for unit tests).
 * @param out    Output feature struct; zeroed by this function on entry.
 */
void ase_feat_extract(const struct ase_frame_state *st, float sr_hz, struct ase_features *out);

/**
 * Serialise the feature struct into a flat float vector for model inference.
 *
 * Packing order is fixed (model weights assume this layout):
 *   [0..7]  band_energy[0..7], [8] centroid_hz, [9] flatness,
 *   [10] rolloff_hz, [11] crest, [12] zcr, [13] rms.
 *
 * @param f    Source features.
 * @param vec  Destination buffer; must hold at least ASE_FEATURE_DIM floats.
 * @param cap  Capacity of vec in elements.
 * @return     ASE_FEATURE_DIM on success, 0 if cap < ASE_FEATURE_DIM.
 */
size_t ase_feat_pack(const struct ase_features *f, float *vec, size_t cap);

/**
 * Safety/security sound-event taxonomy (reference-grade).
 *
 * These four classes cover the primary indoor acoustic hazards targeted by
 * the first-generation classifier.  Integer values are stable and may be
 * stored in persistent event logs or transmitted over the wire.
 */
typedef enum {
	/** Background / no safety event detected.                               */
	ASE_AMBIENT = 0,
	/** Impulsive broadband high-frequency transient (e.g. window break).    */
	ASE_GLASS_BREAK = 1,
	/** Narrowband tonal beep: smoke detector, CO alarm, intruder alarm.     */
	ASE_ALARM = 2,
	/** Human scream or other distress vocalization in the voice band.        */
	ASE_SCREAM = 3,
	/** Sentinel: number of valid event codes; not a real event class.        */
	ASE_EVENT_COUNT
} ase_event_t;

/**
 * Classification result returned by ase_classify_fallback() or an AI model.
 *
 * @note confidence is engine-specific; values from the rule-based fallback
 *       are not directly comparable to probabilities from a trained model.
 */
struct ase_verdict {
	ase_event_t ev;
	float       confidence; /**< 0..1 */
};

/**
 * Deterministic event classifier over the feature vector.  Runs when no AI
 * model is loaded.  Reference-grade threshold rules; customers retrain/retune
 * for their specific acoustic environment and microphone placement.
 */
struct ase_verdict ase_classify_fallback(const struct ase_features *f);

/** Return a stable upper-case ASCII event name suitable for logging. */
const char *ase_event_name(ase_event_t e);

#ifdef __cplusplus
}
#endif

#endif /* ACOUSTIC_EVENT_H */
