/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cold_chain -- pure-C cold-chain integrity metrics for the BME280 example.
 *
 * A slow time series of (temperature, humidity, pressure) is reduced to the
 * metrics a pharma/food cold-chain auditor actually uses:
 *   - excursion time   : how long the temperature spent outside the safe band;
 *   - Mean Kinetic Temp : the single temperature delivering the same cumulative
 *                         thermal stress as the fluctuating profile (ICH Q1A /
 *                         USP <1079>) -- a brief hot spike counts for more than
 *                         its duration, which a plain average would hide;
 *   - dewpoint          : condensation / mould risk when ambient T nears it.
 * Arch-neutral (stdint/math only): builds for native_sim and the M55 alike,
 * and is host-unit-tested.
 */
#ifndef COLD_CHAIN_H
#define COLD_CHAIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -------------------------------------------------------------------------
 * Compile-time constants.
 *
 * All constants are chosen to match the example's BME280 sampling cadence
 * and the ICH Q1A/USP <1079> pharmaceutical cold-chain standard.
 * ------------------------------------------------------------------------- */

/* Window length (samples) and the wall-clock minutes each sample represents.
 * The demo logs one reading per simulated minute, so excursion counts convert
 * directly to minutes.
 * 256 samples = ~4.3 hours at 1 sample/min; enough for a full refrigeration
 * cycle and short enough to fit in M55 SRAM without fragmenting the heap. */
#define CC_WINDOW_N   256
#define CC_SAMPLE_MIN 1.0f

/* Feature vector length: mean/min/max T + mean RH + slope + dewpoint + MKT +
 * excursion-minutes.  Task 2 (classifier) expects exactly this many floats. */
#define CC_FEATURE_DIM 8

/* Arrhenius activation term ΔH/R for MKT.  ΔH = 83.144 kJ/mol (the ICH
 * default for pharmaceutical stability), R = 8.314 J/mol·K → ΔH/R = 10000 K.
 * This is the standard constant used by USP <1079> and ICH Q1A regulators. */
#define CC_DH_OVER_R 10000.0f

/* -------------------------------------------------------------------------
 * Data types.
 * ------------------------------------------------------------------------- */

/** One environmental reading in physical units. */
struct cc_sample {
	float temp_c;      /**< temperature, degrees Celsius. */
	float rh_pct;      /**< relative humidity, percent. */
	float pressure_pa; /**< barometric pressure, pascals (logged, not classified). */
};

/** Sliding window of readings.
 *  @p count saturates at CC_WINDOW_N; the window is "full" once that count is
 *  reached and no further samples are accepted. */
struct cc_window_state {
	/* Plain fill-once array of up to CC_WINDOW_N readings (NOT a ring buffer:
	 * samples are written once per report window and not overwritten). */
	struct cc_sample s[CC_WINDOW_N];
	/* How many samples have been pushed so far; capped at CC_WINDOW_N. */
	uint16_t count;
};

/** Derived cold-chain metrics over the window.
 *  Produced by cc_feat_extract(); consumed by the Task-2 classifier and by
 *  cc_feat_pack() to build the float vector the AI model expects. */
struct cc_features {
	float mean_temp_c;          /**< arithmetic mean temperature. */
	float min_temp_c;           /**< coldest sample. */
	float max_temp_c;           /**< warmest sample. */
	float mean_rh_pct;          /**< mean relative humidity. */
	float temp_slope_c_per_min; /**< warming/cooling trend (>0 = warming). */
	float dewpoint_c;           /**< Magnus dewpoint of the mean T/RH. */
	float mkt_c;                /**< mean kinetic temperature, Celsius. */
	float excursion_min;        /**< minutes spent outside the safe band. */
};

/** Per-product thresholds (a vaccine fridge by default).
 *  All temperature limits are in degrees Celsius; all time limits in minutes.
 *  The caller populates this from NVS or a compile-time default and passes it
 *  to cc_feat_extract(); the library never writes to it. */
struct cc_config {
	float t_lo;                /**< safe-band low edge, Celsius. */
	float t_hi;                /**< safe-band high edge, Celsius. */
	float mkt_limit_c;         /**< MKT above this = cumulative damage. */
	float excursion_min_limit; /**< excursion minutes above this = breach. */
	float dewpoint_margin_c;   /**< T within this of dewpoint = condensation risk. */
};

/* -------------------------------------------------------------------------
 * Window management.
 * ------------------------------------------------------------------------- */

/** Reset the window (count = 0).
 *  Call this once at start-up or whenever a new monitoring session begins. */
void cc_window_reset(struct cc_window_state *st);

/** Append one reading; ignored once the window is full.
 *  Typical call rate: once per CC_SAMPLE_MIN minutes (the demo fires every
 *  simulated minute). */
void cc_window_push(struct cc_window_state *st, struct cc_sample s);

/** True once CC_WINDOW_N readings have been pushed.
 *  The classifier (Task 2) gates on this: it should not run until the window
 *  is full so that every statistic is computed over a complete epoch. */
bool cc_window_full(const struct cc_window_state *st);

/* -------------------------------------------------------------------------
 * Feature extraction and packing.
 * ------------------------------------------------------------------------- */

/** Reduce the window to the cold-chain metrics.  @p cfg supplies the safe band
 *  used for the excursion count.
 *
 * All eight fields of @p out are always written; if the window is empty every
 * field is zero.  Calling on a partially-filled window is legal (useful for
 * early-warning before the window is full). */
void cc_feat_extract(const struct cc_window_state *st,
                     const struct cc_config       *cfg,
                     struct cc_features           *out);

/** Pack the metrics into the AI feature vector; returns the count (CC_FEATURE_DIM).
 *  Returns 0 without writing anything if @p cap < CC_FEATURE_DIM.
 *  Field order matches the training pipeline; do NOT reorder. */
size_t cc_feat_pack(const struct cc_features *f, float *vec, size_t cap);

/* -------------------------------------------------------------------------
 * 4-state integrity classifier.
 *
 * The classifier reduces the rich feature vector to a single actionable state
 * using deterministic thresholds from @p cfg.  Severity levels are ordered:
 * an acute excursion (product currently out of band) is flagged before the
 * cumulative MKT breach, which is flagged before the humidity-driven
 * condensation risk.  This ordering ensures the most urgent condition is
 * always visible to the operator.
 * ------------------------------------------------------------------------- */

/** Integrity state (reference-grade; customers retune the config per product). */
typedef enum {
	CC_OK                = 0, /**< inside band, MKT under limit, no condensation. */
	CC_TEMP_EXCURSION    = 1, /**< acute: mean out of band, or excursion-time over limit. */
	CC_MKT_EXCEEDED      = 2, /**< cumulative thermal damage (MKT over limit). */
	CC_CONDENSATION_RISK = 3, /**< T near dewpoint or very high humidity. */
	CC_STATE_COUNT
} cc_state_t;

/**
 * Classify the integrity state.  Order matters: an ACUTE excursion (currently
 * out of band) is reported before the CUMULATIVE MKT breach, which is reported
 * before the humidity-driven condensation risk.
 */
cc_state_t cc_classify(const struct cc_features *f, const struct cc_config *cfg);

/** Stable upper-case state name for the record. */
const char *cc_state_name(cc_state_t s);

/**
 * Deterministic 0..1 anomaly score = max(excursion depth, MKT overshoot),
 * saturating.  Used when no AI model is loaded.
 */
float cc_anomaly_fallback(const struct cc_features *f, const struct cc_config *cfg);

#ifdef __cplusplus
}
#endif

#endif /* COLD_CHAIN_H */
