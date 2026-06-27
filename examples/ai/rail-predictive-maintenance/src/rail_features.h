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

#define RAIL_WINDOW_N 256
#define RAIL_N_BANDS  8
#define RAIL_ODR_HZ   800.0f
/** 3 scalars (rms, crest, kurtosis) + 8 bands + dom_freq + wavelength. */
#define RAIL_FEATURE_DIM (3 + RAIL_N_BANDS + 2)

/** Accumulating sample window. */
struct rail_feat_state {
	float    samples[RAIL_WINDOW_N];
	uint16_t count;
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

/** True once RAIL_WINDOW_N samples have been pushed. */
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

#ifdef __cplusplus
}
#endif

#endif /* RAIL_FEATURES_H */
