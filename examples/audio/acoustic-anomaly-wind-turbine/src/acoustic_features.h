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

#define ACO_FRAME_N 256
#define ACO_N_BANDS 12
#define ACO_SR_HZ   16000.0f
/** 12 band energies + flatness + centroid + kurtosis + total_rms. */
#define ACO_FEATURE_DIM (ACO_N_BANDS + 4)

struct aco_frame_state {
	float    samples[ACO_FRAME_N];
	uint16_t count;
};

struct aco_features {
	float band_energy[ACO_N_BANDS]; /**< normalised log-band energy (sum 1). */
	float spectral_flatness;        /**< geo-mean/arith-mean: ~1 broadband, ~0 tonal. */
	float spectral_centroid_hz;     /**< magnitude-weighted mean frequency. */
	float kurtosis;                 /**< time-domain 4th moment (impulsiveness). */
	float total_rms;                /**< AC RMS (DC removed). */
};

void   aco_frame_reset(struct aco_frame_state *st);
void   aco_frame_push(struct aco_frame_state *st, float sample);
bool   aco_frame_full(const struct aco_frame_state *st);
void   aco_feat_extract(const struct aco_frame_state *st, float sr_hz, struct aco_features *out);
size_t aco_feat_pack(const struct aco_features *f, float *vec, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ACOUSTIC_FEATURES_H */
