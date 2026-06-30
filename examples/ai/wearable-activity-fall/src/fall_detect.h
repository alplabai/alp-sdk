/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * fall_detect -- pure-C 3-phase fall detector (free-fall -> impact ->
 * post-impact stillness) as a per-sample state machine.  Rule-based, no model
 * (well-understood physics).  Arch-neutral; host-unit-tested.
 *
 * Quick reference -- call sequence:
 *
 *   struct fall_state fs;
 *   fall_reset(&fs);                // once at startup
 *
 *   // per-sample loop at sr_hz:
 *   float amag_g = sqrtf(ax*ax + ay*ay + az*az);
 *   float peak;
 *   if (fall_push(&fs, amag_g, MOT_SR_HZ, &peak)) {
 *       // confirmed fall; peak holds the max |a| during impact phase
 *       alert_caregiver(peak);
 *   }
 *
 * Three-phase summary:
 *   Phase 1 FREEFALL    : |a| < 0.5 g for >= ~80 ms
 *   Phase 2 WAIT_IMPACT : |a| > 2.5 g within ~300 ms after free-fall
 *   Phase 3 POST_IMPACT : near-1g stillness for >= ~1 s
 * All time windows scale with the sr_hz argument to fall_push().
 */
#ifndef FALL_DETECT_H
#define FALL_DETECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Threshold constants
 *
 * FALL_FREEFALL_G : upper bound of |a| during genuine free-fall.  0.5 g is
 *   above true weightlessness (0 g ideal) to tolerate sensor noise and soft
 *   falls from low heights.
 * FALL_IMPACT_G   : lower bound of |a| that constitutes a hard impact.  2.5 g
 *   is above the peak of a normal heel-strike (~2 g) but below typical fall
 *   impacts (3--8 g onto a hard floor).
 * --------------------------------------------------------------------------- */
#define FALL_FREEFALL_G 0.5f /**< |a| below this = free-fall. */
#define FALL_IMPACT_G   2.5f /**< |a| above this = impact. */

/* ---------------------------------------------------------------------------
 * State machine types
 * --------------------------------------------------------------------------- */

/*
 * Phases of the fall detector.  The machine starts and returns to NORMAL;
 * the other three phases represent an in-progress potential fall sequence.
 * FREEFALL and WAIT_IMPACT are transient; POST_IMPACT can last up to ~3 s.
 */
enum fall_phase {
	FALL_PHASE_NORMAL = 0,  /**< no fall candidate in progress. */
	FALL_PHASE_FREEFALL,    /**< accumulating below-0.5g samples. */
	FALL_PHASE_WAIT_IMPACT, /**< free-fall ended; waiting for impact spike. */
	FALL_PHASE_POST_IMPACT, /**< impact seen; waiting for post-fall stillness. */
};

/** Per-instance state for one fall detector.  Initialise with fall_reset(). */
struct fall_state {
	enum fall_phase phase;
	uint16_t        ff_count;    /**< consecutive free-fall samples. */
	uint16_t        wait_count;  /**< samples since free-fall ended. */
	uint16_t        post_count;  /**< samples since impact. */
	uint16_t        still_count; /**< consecutive near-1g samples. */
	float           impact_g;    /**< peak |a| seen during POST_IMPACT. */
};

/* ---------------------------------------------------------------------------
 * API
 * --------------------------------------------------------------------------- */

/** Reset all counters and return the detector to FALL_PHASE_NORMAL.
 *  Call once at startup and after any sensor reconfiguration. */
void fall_reset(struct fall_state *st);

/**
 * Feed one accel-magnitude sample (g).  Returns true exactly on the sample
 * that confirms a fall (free-fall >= ~80 ms -> impact > FALL_IMPACT_G within
 * ~300 ms -> post-impact stillness ~1 s), writing the peak impact to
 * @p impact_g_out (may be NULL).  Self-resets after a confirmed fall or a
 * phase timeout.  @p sr_hz sets all sample-count windows; pass MOT_SR_HZ
 * for the standard 100 Hz configuration.
 */
bool fall_push(struct fall_state *st, float amag_g, float sr_hz, float *impact_g_out);

/** True when a free-fall/impact sequence is in progress (phase != NORMAL).
 *  Useful for telemetry, power management, and unit-test assertions. */
bool fall_is_armed(const struct fall_state *st);

#ifdef __cplusplus
}
#endif

#endif /* FALL_DETECT_H */
