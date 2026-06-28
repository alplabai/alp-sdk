/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rail_features -- pure-C DSP feature extraction for rail-vibration
 * predictive maintenance.  Arch-neutral (stdint/math only): builds for
 * native_sim and the Cortex-M55 alike, and is host-unit-tested.
 *
 * Pipeline role: a sliding window of per-sample vibration magnitude is
 * reduced to a small fixed-length feature vector (the AI classifier's
 * input) plus the physically-meaningful dominant-frequency / rail-
 * wavelength pair.  Corrugation is fixed in wavelength (lambda = v/f),
 * so the speed-normalised wavelength is the speed-invariant feature.
 */
#ifndef RAIL_FEATURES_H
#define RAIL_FEATURES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tuning constants for the feature extraction pipeline.
 *
 * RAIL_WINDOW_N  Sliding window length in samples.
 *   256 samples at 800 Hz = 320 ms of data.  Must be a power of 2
 *   (radix-2 FFT requirement); 256 gives 3.125 Hz per bin at 800 Hz ODR.
 *
 * RAIL_N_BANDS  Number of log-spaced spectral energy bands.
 *   8 bands span bins 1..127 on a log scale -- roughly one octave per
 *   band across the 3.125-400 Hz analysis range.
 *
 * RAIL_ODR_HZ  Default accelerometer output data rate in Hz.
 *   800 Hz gives a Nyquist of 400 Hz, covering the short-pitch
 *   corrugation range (100-400 Hz at typical survey speeds).
 */
#define RAIL_WINDOW_N 256
#define RAIL_N_BANDS  8
#define RAIL_ODR_HZ   800.0f
/** 3 scalars (rms, crest, kurtosis) + 8 bands + dom_freq + wavelength. */
#define RAIL_FEATURE_DIM (3 + RAIL_N_BANDS + 2)

/**
 * Accumulating sample window.
 *
 * count grows from 0 to RAIL_WINDOW_N as samples are pushed.  The
 * feature extractor reads exactly count entries (or RAIL_WINDOW_N when
 * full); the trailing portion of samples[] is not accessed.
 */
struct rail_feat_state {
	float    samples[RAIL_WINDOW_N]; /**< Raw vibration-magnitude samples (m/s^2). */
	uint16_t count;                  /**< Number of valid samples currently held. */
};

/** Extracted per-window features. */
struct rail_features {
	float rms;                       /**< AC RMS (DC removed) -- broadband energy. */
	float crest_factor;              /**< peak/RMS -- impulsive defects. */
	float kurtosis;                  /**< 4th moment -- impulsiveness. */
	float band_energy[RAIL_N_BANDS]; /**< normalised log-band energy. */
	float dom_freq_hz;               /**< frequency of the peak spectral bin. */
	float rail_wavelength_m;         /**< speed/dom_freq (0 when guarded). */
};

/** Reset the window (count = 0). */
void rail_feat_state_reset(struct rail_feat_state *st);

/** Append one vibration-magnitude sample; ignored once the window is full. */
void rail_feat_window_push(struct rail_feat_state *st, float sample);

/**
 * True once RAIL_WINDOW_N samples have been pushed into the window.
 * Poll after each rail_feat_window_push to decide when to call
 * rail_feat_extract and rail_feat_state_reset.
 */
bool rail_feat_window_full(const struct rail_feat_state *st);

/**
 * Reduce a full window to features.  @p odr_hz is the sample rate (Hz),
 * @p speed_mps the train speed (m/s, 0 if unknown).  Safe on a partial
 * window (treats count as the length).
 */
void rail_feat_extract(const struct rail_feat_state *st,
                       float                         odr_hz,
                       float                         speed_mps,
                       struct rail_features         *out);

/**
 * Pack @p f into the AI feature vector.  Writes exactly RAIL_FEATURE_DIM
 * floats when @p cap is large enough; returns the number written.
 */
size_t rail_feat_pack(const struct rail_features *f, float *vec, size_t cap);

/** Rail-defect taxonomy (reference-grade; customers retrain/retune). */
typedef enum {
	RAIL_HEALTHY     = 0, /**< No anomaly: all features below thresholds. */
	RAIL_CORRUGATION = 1, /**< Quasi-periodic corrugation (narrowband spectral peak). */
	RAIL_JOINT_WELD  = 2, /**< Discrete transient at a rail joint or weld. */
	RAIL_ROUGH_RCF   = 3, /**< Elevated broadband energy from RCF surface damage. */
	RAIL_CLASS_COUNT
} rail_class_t;

/**
 * Classifier output: class + 0..1 severity.
 *
 * severity = 0.0 means no detected defect contribution; 1.0 means the
 * feature magnitude is at or above the upper saturation point.  For
 * RAIL_HEALTHY the severity is always 0.0.
 */
struct rail_verdict {
	rail_class_t cls;      /**< Predicted defect class. */
	float        severity; /**< Severity in [0, 1]; 0 = healthy, 1 = max defect. */
};

/**
 * Deterministic rule-of-thumb classifier over the feature vector.  Used
 * when no AI model is loaded (e.g. native_sim) so the demo still produces
 * sensible geotagged output; the AI path overrides it when present.
 */
struct rail_verdict rail_classify_fallback(const struct rail_features *f);

/** Stable upper-case name for a class (for the CSV record). */
const char *rail_class_name(rail_class_t c);

#ifdef __cplusplus
}
#endif

#endif /* RAIL_FEATURES_H */
