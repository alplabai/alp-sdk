/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * acoustic_features -- pure-C per-frame DSP feature extraction for the
 * wind-turbine acoustic anomaly monitor.  Arch-neutral (stdint/math only):
 * builds for native_sim and the Cortex-M55 alike; host-unit-tested.
 */
#ifndef ACOUSTIC_FEATURES_H
#define ACOUSTIC_FEATURES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** FFT and frame-buffer length; must be a power of two for the radix-2 FFT. */
#define ACO_FRAME_N 256
/** Number of logarithmically-spaced spectral bands in the feature vector. */
#define ACO_N_BANDS 12
/** Default codec sample rate; determines the Hz-per-bin mapping in aco_feat_extract(). */
#define ACO_SR_HZ   16000.0f
/** 12 band energies + flatness + centroid + kurtosis + total_rms. */
#define ACO_FEATURE_DIM (ACO_N_BANDS + 4)

/** Per-frame sample accumulator; filled one sample at a time by aco_frame_push(). */
struct aco_frame_state {
	float    samples[ACO_FRAME_N]; /**< Raw audio samples (pre-DC-removal). */
	uint16_t count;                /**< Number of valid samples in [0, ACO_FRAME_N]. */
};

struct aco_features {
	float band_energy[ACO_N_BANDS]; /**< normalised log-band energy (sum 1). */
	float spectral_flatness;        /**< geo-mean/arith-mean: ~1 broadband, ~0 tonal. */
	float spectral_centroid_hz;     /**< magnitude-weighted mean frequency. */
	float kurtosis;                 /**< time-domain 4th moment (impulsiveness). */
	float total_rms;                /**< AC RMS (DC removed). */
};

/**
 * @brief Reset a frame state, discarding any accumulated samples.
 * @param st Frame state to clear; count is set to zero.
 */
void aco_frame_reset(struct aco_frame_state *st);

/**
 * @brief Append one sample to the frame buffer (no-op when the frame is full).
 * @param st     Frame state to update.
 * @param sample Raw audio sample (float, any scale).
 */
void aco_frame_push(struct aco_frame_state *st, float sample);

/**
 * @brief Return true when ACO_FRAME_N samples have been accumulated.
 * @param st Frame state to query.
 */
bool aco_frame_full(const struct aco_frame_state *st);

/**
 * @brief Compute all acoustic features from a filled (or partial) frame.
 *
 * Performs DC removal, time-domain statistics (RMS, kurtosis), an in-place
 * radix-2 FFT, spectral statistics (centroid, flatness) and log-spaced band
 * energies.  All features are written to @p out; @p out is zeroed first so a
 * silent/short frame returns a safe all-zero result.
 *
 * @param st     Frame buffer (should be full; partial frames are zero-padded).
 * @param sr_hz  Codec sample rate in Hz; use ACO_SR_HZ for the default codec.
 * @param out    Output struct; overwritten on every call.
 */
void aco_feat_extract(const struct aco_frame_state *st, float sr_hz, struct aco_features *out);

/**
 * @brief Flatten an aco_features struct into a contiguous float vector.
 *
 * Layout: band_energy[0..ACO_N_BANDS-1], spectral_flatness,
 * spectral_centroid_hz, kurtosis, total_rms (== ACO_FEATURE_DIM elements).
 *
 * @param f   Source features from aco_feat_extract().
 * @param vec Destination buffer; must hold at least ACO_FEATURE_DIM floats.
 * @param cap Capacity of @p vec in elements.
 * @return Number of elements written (== ACO_FEATURE_DIM), or 0 if cap too small.
 */
size_t aco_feat_pack(const struct aco_features *f, float *vec, size_t cap);

/** Healthy-baseline template: per-feature mean + inverse variance. */
struct aco_baseline {
	float mean[ACO_FEATURE_DIM];    /**< Per-feature mean of the healthy distribution. */
	float inv_var[ACO_FEATURE_DIM]; /**< Per-feature 1/sigma^2 of the healthy distribution. */
};

/**
 * Deterministic anomaly score in [0,1]: Mahalanobis-style deviation of @p vec
 * from the healthy @p base, squashed by `1 - exp(-d/ACO_FEATURE_DIM)`.  Runs when
 * no AI model is loaded so the demo still produces a real score.
 */
float aco_anomaly_fallback(const float *vec, size_t n, const struct aco_baseline *base);

#ifdef __cplusplus
}
#endif

#endif /* ACOUSTIC_FEATURES_H */
