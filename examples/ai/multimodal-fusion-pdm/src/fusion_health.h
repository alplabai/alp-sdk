/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * fusion_health -- pure-C cross-modal sensor fusion for motor health.
 *
 * Each of three sensors contributes a compact summary: vibration (ICM-42670),
 * current (INA236), temperature (BME280).  fusion_assess() scores each modality
 * against a healthy baseline, counts how many modalities corroborate, and maps
 * the cross-modal PATTERN to a fault hypothesis:
 *
 *   - a real fault shows in SEVERAL modalities (bearing wear -> vibration AND
 *     heat; overload -> all three), so corroboration RAISES confidence;
 *   - an isolated single-modality blip (a sensor knock, a transient) does NOT
 *     corroborate, so it is flagged UNCORROBORATED at low confidence rather
 *     than raising a false alarm.
 * This is why fusion beats a bank of independent single-sensor thresholds.
 * Arch-neutral (stdint/math only): builds for native_sim and the M55 alike.
 */
#ifndef FUSION_HEALTH_H
#define FUSION_HEALTH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 6 per-modality summary fields + 3 per-modality sub-scores = the AI input. */
#define FUSION_FEATURE_DIM 9
/* Number of fault-hypothesis classes (see fusion_fault_t). */
#define FUSION_FAULT_COUNT 5

/** Compact per-modality summary (computed by main.c from raw sensor reads). */
struct fusion_input {
	float vib_rms;        /**< vibration AC RMS (g). */
	float vib_crest;      /**< vibration crest factor (peak/RMS). */
	float current_a;      /**< mean motor current (A). */
	float current_ripple; /**< current AC ripple RMS (A). */
	float temp_c;         /**< motor/case temperature (deg C). */
	float temp_slope;     /**< temperature trend (deg C/interval). */
};

/** Healthy baseline: nominal value + tolerance per fusion_input field, in the
 *  field order {vib_rms, vib_crest, current_a, current_ripple, temp_c,
 *  temp_slope}.  A field is "anomalous" when |value - nominal| > tol. */
struct fusion_baseline {
	float nominal[6];
	float tol[6];
};

/** Fault hypothesis from the cross-modal pattern. */
typedef enum {
	FUSION_HEALTHY             = 0, /**< no modality anomalous. */
	FUSION_BEARING_WEAR        = 1, /**< vibration + temperature (friction heat). */
	FUSION_ELECTRICAL_FAULT    = 2, /**< current anomalous, vibration normal. */
	FUSION_MECHANICAL_OVERLOAD = 3, /**< all three modalities anomalous. */
	FUSION_UNCORROBORATED      = 4, /**< a single odd modality -> low confidence. */
	FUSION_FAULT_COUNT_ENUM         /* not used; FUSION_FAULT_COUNT is the macro. */
} fusion_fault_t;

/** Fusion verdict. */
struct fusion_result {
	float          health_score;  /**< 0 (healthy) .. 1 (critical), confidence-weighted. */
	fusion_fault_t hypothesis;    /**< cross-modal fault class. */
	float          vib_score;     /**< vibration sub-score (>1 = anomalous). */
	float          current_score; /**< current sub-score. */
	float          temp_score;    /**< temperature sub-score. */
	uint8_t        corroboration; /**< how many modalities are anomalous (0..3). */
};

/** Fuse the per-modality summary against the baseline into a verdict. */
void fusion_assess(const struct fusion_input    *in,
                   const struct fusion_baseline *base,
                   struct fusion_result         *out);

/** Pack the 6 summary fields + 3 sub-scores into the AI feature vector;
 *  returns the count (FUSION_FEATURE_DIM), or 0 if @p cap is too small. */
size_t
fusion_pack(const struct fusion_result *r, const struct fusion_input *in, float *vec, size_t cap);

/** Stable upper-case hypothesis name for the record. */
const char *fusion_fault_name(fusion_fault_t f);

#ifdef __cplusplus
}
#endif

#endif /* FUSION_HEALTH_H */
