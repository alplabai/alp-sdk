/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * acoustic_event -- pure-C per-frame DSP feature extraction for the acoustic
 * safety-event classifier.  Arch-neutral (stdint/math only): builds for
 * native_sim and the Cortex-M55 alike; host-unit-tested.
 */
#ifndef ACOUSTIC_EVENT_H
#define ACOUSTIC_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ASE_FRAME_N 512
#define ASE_SR_HZ   16000.0f
#define ASE_N_BANDS 8
/** 8 band energies + centroid + flatness + rolloff + crest + zcr + rms. */
#define ASE_FEATURE_DIM 14

struct ase_frame_state {
	float    samples[ASE_FRAME_N];
	uint16_t count;
};

struct ase_features {
	float band_energy[ASE_N_BANDS]; /**< normalised log-band energy (sum 1). */
	float centroid_hz;              /**< magnitude-weighted mean frequency. */
	float flatness;                 /**< geo/arith mean: ~1 broadband, ~0 tonal. */
	float rolloff_hz;               /**< freq below which 85% of energy lies. */
	float crest;                    /**< peak/RMS (impulsiveness). */
	float zcr;                      /**< zero-crossing rate (0..1). */
	float rms;                      /**< AC RMS (DC removed). */
};

void   ase_frame_reset(struct ase_frame_state *st);
void   ase_frame_push(struct ase_frame_state *st, float sample);
bool   ase_frame_full(const struct ase_frame_state *st);
void   ase_feat_extract(const struct ase_frame_state *st, float sr_hz, struct ase_features *out);
size_t ase_feat_pack(const struct ase_features *f, float *vec, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* ACOUSTIC_EVENT_H */
