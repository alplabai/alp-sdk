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

/** Operating-state taxonomy (reference-grade; customers retune the config). */
typedef enum {
	CURR_OFF      = 0,
	CURR_NORMAL   = 1,
	CURR_INRUSH   = 2,
	CURR_OVERLOAD = 3,
	CURR_STALL    = 4,
	CURR_STATE_COUNT
} curr_state_t;

/** Motor-specific thresholds (Amps). */
struct curr_config {
	float off_a;          /**< below this mean current = OFF. */
	float overload_a;     /**< above this mean current = OVERLOAD/STALL. */
	float ripple_min_a;   /**< AC ripple below this at high current = STALL. */
	float inrush_slope_a; /**< slope below -this = decaying inrush. */
};

/** Classify the operating state from the features + config. */
curr_state_t current_classify(const struct curr_features *f, const struct curr_config *cfg);

/** Stable upper-case state name for the record. */
const char *curr_state_name(curr_state_t s);

/**
 * Deterministic 0..1 anomaly score (overcurrent severity, saturating high on a
 * stall).  Used when no AI model is loaded.
 */
float curr_anomaly_fallback(const struct curr_features *f, const struct curr_config *cfg);

#ifdef __cplusplus
}
#endif

#endif /* CURRENT_FEATURES_H */
