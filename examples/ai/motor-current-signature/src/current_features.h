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

/*
 * Sampling and window parameters
 * --------------------------------
 * CURR_SR_HZ = 200 Hz gives a Nyquist ceiling of 100 Hz -- sufficient to
 * resolve commutation ripple on the low-pole-count motors targeted here.
 *
 * CURR_WINDOW_N = 256 samples spans 1.28 s: long enough for several
 * commutation cycles at low speed, short enough for sub-2-second latency.
 * 256 is a power of 2 so the FFT needs no zero-padding of its own.
 *
 * FFT bin resolution = CURR_SR_HZ / CURR_WINDOW_N = 200 / 256 ≈ 0.78 Hz.
 */
#define CURR_WINDOW_N 256
#define CURR_SR_HZ    200.0f
/** mean_current + rms_ac + crest + slope + mean_power + mean_bus + ripple_freq. */
#define CURR_FEATURE_DIM 7

/** One INA236 sample, SI units. */
struct curr_sample {
	float current_a; /**< Phase current in Amperes (shunt-sense, signed). */
	float bus_v;     /**< Bus voltage in Volts. */
	float power_w;   /**< Instantaneous power in Watts (current_a × bus_v). */
};

/*
 * curr_window_state -- fixed-length sample accumulator.
 *
 * Holds up to CURR_WINDOW_N raw samples collected at CURR_SR_HZ.  When
 * count == CURR_WINDOW_N the window is ready for feature extraction.  Push
 * silently stops at capacity; call curr_window_reset() to begin the next
 * window after processing the previous one.
 */
struct curr_window_state {
	struct curr_sample s[CURR_WINDOW_N]; /**< Sample buffer, filled front-to-back. */
	uint16_t           count;            /**< Number of valid samples stored so far. */
};

/*
 * curr_features -- one feature vector derived from a complete window.
 *
 * All fields are in SI units.  The vector is designed to feed both the
 * deterministic rule-based classifier (current_classify) and a trained ML
 * model delivered via the alp model pipeline.  Field order is fixed and must
 * match the training-time feature order used by any downstream model (see
 * curr_feat_pack for the serialisation order).
 */
struct curr_features {
	float mean_current_a; /**< DC mean current over the window (A). */
	float rms_ac_a;       /**< RMS of (I − mean): commutation ripple magnitude (A). */
	float crest;          /**< peak|I−mean| / rms_ac; 0 when rms_ac is negligible. */
	float slope_a;        /**< last-quarter mean − first-quarter mean (A); < 0 during inrush. */
	float mean_power_w;   /**< DC mean power over the window (W). */
	float mean_bus_v;     /**< DC mean bus voltage over the window (V). */
	float ripple_freq_hz; /**< Frequency of the dominant FFT bin of the AC current (Hz). */
};

/**
 * @brief Reset a window accumulator, discarding all stored samples.
 *
 * @param st  Window state to reset.  Must not be NULL.
 */
void curr_window_reset(struct curr_window_state *st);

/**
 * @brief Push one sample into the window accumulator.
 *
 * Silently drops the sample once the window is full.  Check
 * curr_window_full() to detect when the window is ready for extraction.
 *
 * @param st  Window accumulator.
 * @param s   Sample to append (copied by value).
 */
void curr_window_push(struct curr_window_state *st, struct curr_sample s);

/**
 * @brief Return true when the window holds CURR_WINDOW_N samples.
 *
 * @param st  Window accumulator (read-only).
 * @return    true if full, false otherwise.
 */
bool curr_window_full(const struct curr_window_state *st);

/**
 * @brief Extract all features from a (possibly partial) window.
 *
 * Computes DC means, AC ripple RMS, crest factor, inrush slope, and the
 * dominant ripple frequency (via the <alp/dsp.h> FFT chain) and writes them
 * into @p out.  A partial window (count < CURR_WINDOW_N) is zero-padded for
 * the FFT.  If the window is empty all output fields are zero.
 *
 * @param st      Window accumulator (read-only).
 * @param sr_hz   Sampling rate in Hz; converts FFT bin index to frequency.
 *                Pass CURR_SR_HZ unless the hardware rate has changed.
 * @param out     Output feature struct; fully overwritten on return.
 */
void curr_feat_extract(const struct curr_window_state *st, float sr_hz, struct curr_features *out);

/**
 * @brief Serialise features into a flat float array for model inference.
 *
 * Writes exactly CURR_FEATURE_DIM floats into @p vec in the fixed order:
 * mean_current_a, rms_ac_a, crest, slope_a, mean_power_w, mean_bus_v,
 * ripple_freq_hz.  This order must match the feature order used at model
 * training time.
 *
 * @param f    Feature struct to serialise.
 * @param vec  Output buffer.  Must have capacity >= CURR_FEATURE_DIM.
 * @param cap  Capacity of @p vec in number of floats.
 * @return     Number of floats written (== CURR_FEATURE_DIM), or 0 if
 *             @p cap < CURR_FEATURE_DIM.
 */
size_t curr_feat_pack(const struct curr_features *f, float *vec, size_t cap);

/** Operating-state taxonomy (reference-grade; customers retune the config). */
typedef enum {
	CURR_OFF      = 0, /**< Motor de-energised; mean current below off_a. */
	CURR_NORMAL   = 1, /**< Running within spec; current and ripple nominal. */
	CURR_INRUSH   = 2, /**< Decaying startup spike; slope_a < −inrush_slope_a. */
	CURR_OVERLOAD = 3, /**< High current, rotor turning; commutation ripple present. */
	CURR_STALL    = 4, /**< High current, no ripple; rotor has stopped. */
	CURR_STATE_COUNT   /**< Sentinel; not a valid operating state. */
} curr_state_t;

/** Motor-specific thresholds (all in Amperes unless noted). */
struct curr_config {
	float off_a;          /**< Mean current below this → motor is OFF. */
	float overload_a;     /**< Mean current above this → OVERLOAD or STALL. */
	float ripple_min_a;   /**< rms_ac below this at high current → STALL (no commutation). */
	float inrush_slope_a; /**< slope_a below −this magnitude → INRUSH. */
};

/**
 * @brief Classify the operating state from a feature vector.
 *
 * Implements the deterministic five-state MCSA classifier.  Branch priority:
 * OFF → INRUSH (slope) → STALL/OVERLOAD (ripple discriminant) → NORMAL.
 * See current_features.c for the full rationale of the branch ordering.
 *
 * @param f    Extracted feature vector (from curr_feat_extract).
 * @param cfg  Motor-specific threshold configuration.
 * @return     One of: CURR_OFF, CURR_NORMAL, CURR_INRUSH, CURR_OVERLOAD,
 *             CURR_STALL.
 */
curr_state_t current_classify(const struct curr_features *f, const struct curr_config *cfg);

/**
 * @brief Return the upper-case ASCII name of a state (e.g. "STALL").
 *
 * @param s  State value.
 * @return   Null-terminated constant string; never NULL.
 */
const char *curr_state_name(curr_state_t s);

/**
 * @brief Deterministic 0..1 anomaly score for use when no AI model is loaded.
 *
 * Scores 0.0 within normal bounds, rises proportionally with overcurrent
 * severity, and saturates to 0.9 on a detected stall (high current with
 * near-zero ripple) -- the most thermally destructive common fault.
 * Output is hard-clamped to [0.0, 1.0].
 *
 * @param f    Extracted feature vector.
 * @param cfg  Motor-specific thresholds.
 * @return     Anomaly severity in [0.0, 1.0].
 */
float curr_anomaly_fallback(const struct curr_features *f, const struct curr_config *cfg);

#ifdef __cplusplus
}
#endif

#endif /* CURRENT_FEATURES_H */
