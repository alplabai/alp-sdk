/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * fall_detect -- pure-C 3-phase fall detector (free-fall -> impact ->
 * post-impact stillness) as a per-sample state machine.  Rule-based, no model
 * (well-understood physics).  Arch-neutral; host-unit-tested.
 */
#ifndef FALL_DETECT_H
#define FALL_DETECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FALL_FREEFALL_G 0.5f /**< |a| below this = free-fall. */
#define FALL_IMPACT_G   2.5f /**< |a| above this = impact. */

enum fall_phase {
	FALL_PHASE_NORMAL = 0,
	FALL_PHASE_FREEFALL,
	FALL_PHASE_WAIT_IMPACT,
	FALL_PHASE_POST_IMPACT,
};

struct fall_state {
	enum fall_phase phase;
	uint16_t        ff_count;    /**< consecutive free-fall samples. */
	uint16_t        wait_count;  /**< samples since free-fall ended. */
	uint16_t        post_count;  /**< samples since impact. */
	uint16_t        still_count; /**< consecutive near-1g samples. */
	float           impact_g;    /**< peak |a| of the impact. */
};

/** Reset to the NORMAL phase. */
void fall_reset(struct fall_state *st);

/**
 * Feed one accel-magnitude sample (g).  Returns true exactly on the sample that
 * confirms a fall (free-fall >= ~80 ms -> impact > FALL_IMPACT_G within ~300 ms
 * -> post-impact stillness ~1 s), writing the peak impact to @p impact_g_out.
 * Self-resets after a confirmed fall or a phase timeout.  @p sr_hz sets the
 * sample-count windows.
 */
bool fall_push(struct fall_state *st, float amag_g, float sr_hz, float *impact_g_out);

/** True when a free-fall/impact sequence is in progress (telemetry/tests). */
bool fall_is_armed(const struct fall_state *st);

#ifdef __cplusplus
}
#endif

#endif /* FALL_DETECT_H */
