/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * fusion_health implementation -- see fusion_health.h for the full API doc.
 *
 * Algorithm overview
 * ------------------
 * 1. Normalise each of the 6 summary fields to a dimensionless deviation score:
 *      dev[i] = |value - nominal| / tolerance
 *    so dev=0 means exactly at the healthy baseline, dev=1 is at the edge of
 *    tolerance, and dev>1 is anomalous.
 *
 * 2. Reduce each modality's two fields to one sub-score by taking the MAX --
 *    one bad field is sufficient to call the whole modality anomalous.
 *
 * 3. Count how many of the three modalities exceed the anomaly threshold (>1).
 *    This "corroboration count" is the key insight: real mechanical faults leave
 *    fingerprints in MULTIPLE physics channels simultaneously (a bearing wearing
 *    out raises both vibration AND temperature); sensor noise or transients are
 *    usually confined to ONE channel.
 *
 * 4. Map the {vib_hi, cur_hi, temp_hi} boolean pattern to a fault hypothesis
 *    that describes the physical mechanism.
 *
 * 5. Compute a confidence-weighted health score:
 *      severity  = saturate((max_sub - 1) / 2, 0, 1)
 *      confidence = 0 (healthy) | 0.5 (uncorroborated) | 1.0 (corroborated)
 *      health_score = severity * confidence
 */
#include "fusion_health.h"

#include <math.h>

/* A modality counts as anomalous once its sub-score exceeds this -- i.e. one of
 * its fields has deviated by more than its tolerance from the healthy nominal. */
#define FUSION_ANOMALY_THRESH 1.0f

void fusion_assess(const struct fusion_input    *in,
                   const struct fusion_baseline *base,
                   struct fusion_result         *out)
{
	/* Lay the 6 summary fields out in the same order as the baseline arrays so
	 * we can score them with one loop rather than six separate expressions.
	 * Field order: vib_rms, vib_crest, current_a, current_ripple, temp_c, temp_slope. */
	const float v[6] = { in->vib_rms,        in->vib_crest, in->current_a,
		                 in->current_ripple, in->temp_c,    in->temp_slope };

	/* Per-field normalised deviation: how many tolerances away from nominal.
	 *   dev = 0 -> exactly nominal
	 *   dev = 1 -> at the tolerance boundary (just inside anomaly zone)
	 *   dev > 1 -> anomalous
	 * Guard against a zero tolerance (sensor not configured) by returning 0. */
	float dev[6];
	for (int i = 0; i < 6; i++) {
		dev[i] = (base->tol[i] > 1e-9f) ? (fabsf(v[i] - base->nominal[i]) / base->tol[i]) : 0.0f;
	}

	/* Each modality's sub-score is the worst (max) of its two fields' deviations.
	 * One bad field is enough to call the modality anomalous; we never average
	 * a bad field with a good one because that would mask spiky anomalies. */
	out->vib_score     = fmaxf(dev[0], dev[1]); /* vib_rms vs vib_crest */
	out->current_score = fmaxf(dev[2], dev[3]); /* current_a vs current_ripple */
	out->temp_score    = fmaxf(dev[4], dev[5]); /* temp_c vs temp_slope */

	/* Which modalities are anomalous, and how many corroborate.
	 * Corroboration is the cross-modal evidence count: 0 = healthy, 1 = suspicious
	 * (single sensor, could be noise), 2-3 = likely real fault. */
	int vib_hi         = (out->vib_score > FUSION_ANOMALY_THRESH);
	int cur_hi         = (out->current_score > FUSION_ANOMALY_THRESH);
	int temp_hi        = (out->temp_score > FUSION_ANOMALY_THRESH);
	out->corroboration = (uint8_t)(vib_hi + cur_hi + temp_hi);

	/* Map the cross-modal pattern to a hypothesis.
	 *
	 * Priority order matters: HEALTHY is checked FIRST so a corroboration=0
	 * window is never mislabeled.  MECHANICAL_OVERLOAD (all three) is next so
	 * it is never eclipsed by a two-modality sub-pattern.
	 *
	 * Pattern table:
	 *   vib  cur  temp | hypothesis
	 *   ---  ---  ---- | ----------
	 *    0    0    0   | HEALTHY
	 *    1    1    1   | MECHANICAL_OVERLOAD (all channels stressed)
	 *    1    0    1   | BEARING_WEAR        (friction: heat + vibration)
	 *    0    1    *   | ELECTRICAL_FAULT    (winding/supply, no vibration)
	 *    1    0    0   | UNCORROBORATED      (single channel, likely noise)
	 */
	if (out->corroboration == 0) {
		/* No modality anomalous: the motor is running within spec. */
		out->hypothesis = FUSION_HEALTHY;
	} else if (vib_hi && cur_hi && temp_hi) {
		/* Vibration + electrical load + heat all elevated = the motor is being
		 * driven beyond its rating.  All three physics channels are stressed. */
		out->hypothesis = FUSION_MECHANICAL_OVERLOAD;
	} else if (vib_hi && temp_hi && !cur_hi) {
		/* Mechanical roughness + friction heat, electrical draw still normal =
		 * the classic bearing-wear signature.  The increased friction raises
		 * case temperature and excites higher-frequency vibration, but the
		 * electromagnetic load on the winding has not changed yet. */
		out->hypothesis = FUSION_BEARING_WEAR;
	} else if (cur_hi && !vib_hi) {
		/* Current anomaly without mechanical vibration = a winding/supply issue.
		 * Over-current or ripple with normal vibration points to the electrical
		 * subsystem (imbalance, winding fault, supply droop). */
		out->hypothesis = FUSION_ELECTRICAL_FAULT;
	} else {
		/* A lone or odd modality (e.g. vibration only) -- nothing corroborates,
		 * so this is more likely noise / a sensor knock than a real fault.
		 * Flagging it UNCORROBORATED keeps it visible without raising an alarm. */
		out->hypothesis = FUSION_UNCORROBORATED;
	}

	/* Compute the confidence-weighted health score.
	 *
	 * Severity maps the worst sub-score onto [0, 1]:
	 *   sub=1.0  -> severity=0   (just at tolerance edge, no danger yet)
	 *   sub=3.0  -> severity=1   (saturated: 3× tolerance = critical)
	 *   sub between -> linear interpolation
	 *
	 * Confidence reflects how much cross-modal evidence we have:
	 *   0.0  = healthy (no score at all)
	 *   0.5  = uncorroborated single modality (discount for possible noise)
	 *   1.0  = corroborated by ≥2 modalities (high confidence real fault)
	 *
	 * Final: health_score = severity × confidence, range [0, 1]. */
	float max_sub  = fmaxf(out->vib_score, fmaxf(out->current_score, out->temp_score));
	float severity = (max_sub - 1.0f) / 2.0f; /* linear: sub=1->0, sub=3->1 */
	if (severity < 0.0f) {
		severity = 0.0f; /* clamp below: nominal window gives negative raw */
	}
	if (severity > 1.0f) {
		severity = 1.0f; /* clamp above: extreme deviations saturate at 1 */
	}
	float confidence;
	switch (out->hypothesis) {
	case FUSION_HEALTHY:
		confidence = 0.0f; /* no fault -> no score. */
		break;
	case FUSION_UNCORROBORATED:
		confidence = 0.5f; /* single modality -> half-weight (likely noise). */
		break;
	default:
		confidence = 1.0f; /* corroborated fault -> full weight. */
		break;
	}
	out->health_score = severity * confidence;
}

size_t
fusion_pack(const struct fusion_result *r, const struct fusion_input *in, float *vec, size_t cap)
{
	/* Safety check: caller must provide a buffer large enough for the full
	 * feature vector.  Returning 0 (not FUSION_FEATURE_DIM) signals the error
	 * without crashing -- the AI pipeline can treat 0 as "no update this tick". */
	if (cap < (size_t)FUSION_FEATURE_DIM) {
		return 0;
	}
	size_t i = 0;
	/* First 6 elements: the raw per-modality summary fields.
	 * These are the physical measurements that the classifier sees directly. */
	vec[i++] = in->vib_rms;
	vec[i++] = in->vib_crest;
	vec[i++] = in->current_a;
	vec[i++] = in->current_ripple;
	vec[i++] = in->temp_c;
	vec[i++] = in->temp_slope;
	/* Last 3 elements: the derived sub-scores (normalised deviations).
	 * Providing these pre-computed features gives the AI model a head start:
	 * the rule-based normalisation already encodes domain knowledge about
	 * what "anomalous" means for each sensor, so the model can focus on
	 * learning the cross-modal interaction patterns. */
	vec[i++] = r->vib_score;
	vec[i++] = r->current_score;
	vec[i++] = r->temp_score;
	return i; /* == FUSION_FEATURE_DIM, confirms full vector written */
}

const char *fusion_fault_name(fusion_fault_t f)
{
	/* Return a stable, upper-case string per hypothesis class.
	 * Stable = the string is a compile-time literal that never changes address,
	 * so callers can cache the pointer safely across assessment cycles.
	 * Upper-case = matches the enum identifier for easy grepping in logs. */
	switch (f) {
	case FUSION_HEALTHY:
		return "HEALTHY";
	case FUSION_BEARING_WEAR:
		return "BEARING_WEAR";
	case FUSION_ELECTRICAL_FAULT:
		return "ELECTRICAL_FAULT";
	case FUSION_MECHANICAL_OVERLOAD:
		return "MECHANICAL_OVERLOAD";
	case FUSION_UNCORROBORATED:
		return "UNCORROBORATED";
	default:
		/* Defensive: covers future enum extensions that miss this switch. */
		return "UNKNOWN";
	}
}
