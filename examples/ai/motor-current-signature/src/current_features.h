/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * current_features -- pure-C windowed current/voltage/power feature extraction
 * for the DC motor current-signature health example.  Arch-neutral (stdint/math
 * only): builds for native_sim and the Cortex-M55 alike; host-unit-tested.
 */
#ifndef CURRENT_FEATURES_H
#define CURRENT_FEATURES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CURR_WINDOW_N 256
#define CURR_SR_HZ    200.0f
/** mean_current + rms_ac + crest + slope + mean_power + mean_bus + ripple_freq. */
#define CURR_FEATURE_DIM 7

/** One INA236 sample, SI units. */
struct curr_sample {
	float current_a;
	float bus_v;
	float power_w;
};

struct curr_window_state {
	struct curr_sample s[CURR_WINDOW_N];
	uint16_t           count;
};

struct curr_features {
	float mean_current_a;
	float rms_ac_a; /**< RMS of (current - mean): the ripple magnitude. */
	float crest;    /**< peak|current-mean| / rms_ac (0 when no ripple). */
	float slope_a;  /**< last-quarter mean - first-quarter mean (inrush < 0). */
	float mean_power_w;
	float mean_bus_v;
	float ripple_freq_hz; /**< dominant FFT bin of the AC current. */
};

void curr_window_reset(struct curr_window_state *st);
void curr_window_push(struct curr_window_state *st, struct curr_sample s);
bool curr_window_full(const struct curr_window_state *st);
void curr_feat_extract(const struct curr_window_state *st, float sr_hz, struct curr_features *out);
size_t curr_feat_pack(const struct curr_features *f, float *vec, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* CURRENT_FEATURES_H */
