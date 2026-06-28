/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * fall_detect implementation -- see fall_detect.h.
 *
 * Classic three-phase, rule-based fall detector implemented as a per-sample
 * state machine.  The algorithm is based on the approach documented in the
 * wearable health literature (Kangas et al.; Bourke et al.) and operates
 * reliably on wrist-worn accelerometers at 50--200 Hz.
 *
 * Why three phases instead of a single impact threshold?
 * -------------------------------------------------------
 * A naive impact detector (amag > FALL_IMPACT_G) generates many false
 * positives because ordinary activities easily exceed 2.5 g:
 *   - Sitting down forcefully                   (~3--4 g impact)
 *   - Placing a rigid wrist on a table          (~3 g)
 *   - Dropped device landing screen-down        (any peak)
 *   - Running heel-strike on hard pavement      (~2.5--4 g)
 *
 * Adding the preceding free-fall gate eliminates almost all non-fall impacts
 * because genuine falls are the only everyday event that produces BOTH:
 *   a) near-weightlessness (|a| < 0.5 g) for a sustained interval, AND
 *   b) a large impact shortly after.
 *
 * The post-impact stillness phase confirms the subject landed and remained
 * on the ground, ruling out stumble-and-recover events where the person
 * catches themselves (impact present, but immediately followed by renewed
 * dynamic motion rather than rest).
 *
 * State machine overview
 * -----------------------
 *
 *   NORMAL
 *     |  amag < 0.5g
 *     v
 *   FREEFALL ──────── (duration < ff_min) ──────────────────────> NORMAL
 *     |  amag >= 0.5g AND duration >= ff_min
 *     |
 *     +──── (amag > 2.5g on the first rising sample) ──────────> POST_IMPACT
 *     |
 *     v
 *   WAIT_IMPACT
 *     |  amag > 2.5g within impact_win samples
 *     v
 *   POST_IMPACT ───── (post_count > post_max, no settling) ────> NORMAL
 *     |  still_count >= still_min  (~1 s near-1g)
 *     v
 *   NORMAL  +  fire (return true, write peak impact_g)
 *
 * Phase timing at 100 Hz (sr_hz = MOT_SR_HZ)
 * --------------------------------------------
 *   ff_min     = round(0.08 * 100) =   8 samples   (~80 ms minimum free-fall)
 *   impact_win = round(0.30 * 100) =  30 samples   (~300 ms to find the impact)
 *   still_min  = round(1.00 * 100) = 100 samples   (~1 s post-impact stillness)
 *   post_max   = round(3.00 * 100) = 300 samples   (~3 s maximum wait)
 *
 * All counts scale linearly with sr_hz so the thresholds are sample-rate
 * independent in physical time.
 *
 * Single-fire + self-reset guarantee
 * ------------------------------------
 * fall_push() returns true EXACTLY ONCE per confirmed fall event (on the
 * sample where still_count reaches still_min), then immediately calls
 * fall_reset() to return to NORMAL.  The post_max timeout also calls
 * fall_reset() to prevent the machine from staying armed indefinitely if the
 * subject never settles after impact (e.g. rolls down a slope).  The caller
 * does NOT need to reset the state machine after receiving a confirmed fall.
 *
 * False-positive rejection summary
 * ----------------------------------
 *   Scenario                  ff gate   impact gate  stillness gate
 *   Hard table-tap            none      PASS         FAIL (no rest)   -> no fire
 *   Forceful sit-down         FAIL      PASS         N/A              -> no fire
 *   Running heel-strike       FAIL      PASS         N/A              -> no fire
 *   Arm-swing trough (<80ms)  FAIL      ---          N/A              -> no fire
 *   Stumble + recover         PASS      PASS         FAIL (resumes)   -> no fire
 *   Genuine fall              PASS      PASS         PASS             -> FIRE
 */
#include "fall_detect.h"

#include <math.h>

/* ===========================================================================
 * fall_reset  --  unconditional state machine reset
 *
 * Zeros all phase counters and clears the cached peak impact reading.  Must
 * be called on startup before the first fall_push() invocation.  Also called
 * internally after:
 *   - A confirmed fall (single-fire guarantee).
 *   - A free-fall interval that was too short (< ff_min samples).
 *   - The impact window expiring without detecting an impact.
 *   - The post-impact window timing out without settling.
 * =========================================================================== */
void fall_reset(struct fall_state *st)
{
	st->phase       = FALL_PHASE_NORMAL;
	st->ff_count    = 0;
	st->wait_count  = 0;
	st->post_count  = 0;
	st->still_count = 0;
	st->impact_g    = 0.0f;
}

/* ===========================================================================
 * fall_is_armed  --  query whether a fall sequence is in progress
 *
 * Returns true in FREEFALL, WAIT_IMPACT, or POST_IMPACT.  Useful for:
 *   - Telemetry: flag raw samples for closer review during a potential fall.
 *   - Unit tests: assert the state machine entered the expected phase without
 *     having to synthesise a complete three-phase stimulus.
 *   - Power management: suppress deep sleep while a fall may be in progress.
 * =========================================================================== */
bool fall_is_armed(const struct fall_state *st)
{
	return st->phase != FALL_PHASE_NORMAL;
}

/* ===========================================================================
 * fall_push  --  feed one accel-magnitude sample; advance the state machine
 *
 * @param amag_g       Euclidean magnitude of the accelerometer vector in g.
 *                     Caller computes: sqrtf(ax*ax + ay*ay + az*az).
 * @param sr_hz        Sample rate in Hz.  All phase windows are derived from
 *                     this value at call time; it may change between windows.
 * @param impact_g_out Optional.  On a confirmed fall, receives the peak |a|
 *                     recorded during the POST_IMPACT phase.  May be NULL.
 * @return             true exactly once per confirmed fall, false otherwise.
 *
 * Detailed phase transition notes
 * --------------------------------
 *
 * NORMAL → FREEFALL
 *   Any sample with amag_g < FALL_FREEFALL_G (0.5 g) triggers the
 *   free-fall phase.  0.5 g is above true weightlessness (0 g ideal) to
 *   tolerate sensor noise and soft falls from low heights.  ff_count is
 *   initialised to 1 because this first sample already qualifies.
 *
 * FREEFALL: accumulate or abort
 *   Samples below 0.5 g continue to increment ff_count.
 *   On the first sample that rises above 0.5 g:
 *     - ff_count < ff_min  : the excursion was too short to be a real fall
 *       (e.g. an arm-swing trough briefly grazes <0.5 g, typically < 50 ms).
 *       Reset to NORMAL without alerting the caller.
 *     - ff_count >= ff_min : valid free-fall ended.  Transition to
 *       WAIT_IMPACT.  The rising sample is tested immediately for impact
 *       because the impact can arrive on the very first post-freefall sample
 *       (zero-latency hard landing on a rigid floor).
 *
 * WAIT_IMPACT: search for the impact spike
 *   Increment wait_count each sample.  Accept any sample above FALL_IMPACT_G
 *   (2.5 g) as the impact; enter POST_IMPACT.  If wait_count exceeds
 *   impact_win (~300 ms) with no impact: the free-fall was real but the
 *   person may have landed on an extremely soft surface.  Reset to NORMAL
 *   (no confirmed fall -- the post-fall injury risk is low anyway).
 *
 * POST_IMPACT: wait for the body to settle
 *   - Track the running peak impact (impact_g) across all samples in this
 *     phase; a body can bounce, causing several high-g spikes.
 *   - Count consecutive samples within 0.3 g of 1 g (the still-on-ground
 *     condition) as "still".  The 0.3 g tolerance accepts minor breathing
 *     motion and sensor vibration.  Any sample outside the band resets the
 *     still_count (rolling, convulsing, rescuer moving the subject).
 *   - still_count >= still_min (~1 s): confirmed fall.  Write the peak
 *     impact to *impact_g_out, call fall_reset() (single-fire), return true.
 *   - post_count > post_max (~3 s): person never settled.  Reset silently;
 *     the caller gets no notification (device may have been dropped and is
 *     vibrating on a surface, or the subject recovered dynamically).
 * =========================================================================== */
bool fall_push(struct fall_state *st, float amag_g, float sr_hz, float *impact_g_out)
{
	/* Phase windows in samples, derived from the sample rate.  All constants
	 * are inline so the compiler can constant-fold when sr_hz is a literal. */
	const uint16_t ff_min     = (uint16_t)(0.08f * sr_hz); /* >= ~80 ms free-fall */
	const uint16_t impact_win = (uint16_t)(0.30f * sr_hz); /* impact within ~300 ms */
	const uint16_t still_min  = (uint16_t)(1.00f * sr_hz); /* ~1 s stillness */
	const uint16_t post_max   = (uint16_t)(3.00f * sr_hz); /* give up after ~3 s */

	switch (st->phase) {
	/* ---------------------------------------------------------------------- */
	case FALL_PHASE_NORMAL:
		/* Enter the free-fall phase on the first below-threshold sample.
		 * Initialise ff_count to 1: this sample already counts toward ff_min. */
		if (amag_g < FALL_FREEFALL_G) {
			st->phase    = FALL_PHASE_FREEFALL;
			st->ff_count = 1;
		}
		break;

	/* ---------------------------------------------------------------------- */
	case FALL_PHASE_FREEFALL:
		if (amag_g < FALL_FREEFALL_G) {
			/* Still in potential free-fall: count this sample. */
			st->ff_count++;
		} else if (st->ff_count >= ff_min) {
			/* Valid free-fall ended; look for the impact. */
			st->phase      = FALL_PHASE_WAIT_IMPACT;
			st->wait_count = 0;
			st->impact_g   = 0.0f;
			/* This same sample may already be the impact.
			 * Test it now so a zero-latency hard landing is not missed. */
			if (amag_g > FALL_IMPACT_G) {
				st->impact_g    = amag_g;
				st->phase       = FALL_PHASE_POST_IMPACT;
				st->post_count  = 0;
				st->still_count = 0;
			}
		} else {
			st->phase = FALL_PHASE_NORMAL; /* free-fall too short */
		}
		break;

	/* ---------------------------------------------------------------------- */
	case FALL_PHASE_WAIT_IMPACT:
		st->wait_count++;
		if (amag_g > FALL_IMPACT_G) {
			/* Impact detected: record the initial peak, start the post-impact
			 * stillness measurement. */
			st->impact_g    = amag_g;
			st->phase       = FALL_PHASE_POST_IMPACT;
			st->post_count  = 0;
			st->still_count = 0;
		} else if (st->wait_count > impact_win) {
			st->phase = FALL_PHASE_NORMAL; /* no impact after free-fall */
		}
		break;

	/* ---------------------------------------------------------------------- */
	case FALL_PHASE_POST_IMPACT:
		st->post_count++;
		/* Track the peak impact force: report the worst spike across any
		 * bounces or secondary impacts during the post-impact window. */
		if (amag_g > st->impact_g) {
			st->impact_g = amag_g; /* track the spike peak. */
		}
		/* Count consecutive near-1g samples as "settled on the ground".
		 * |amag_g - 1.0| < 0.3 means gravity dominates the reading.
		 * Any sample outside that band resets the counter (person rolling,
		 * rescuer picking them up, or convulsion). */
		if (fabsf(amag_g - 1.0f) < 0.3f) {
			st->still_count++;
		} else {
			st->still_count = 0;
		}
		if (st->still_count >= still_min) {
			/* CONFIRMED FALL.  Capture peak, self-reset, return true.
			 * fall_reset() must be called before returning so subsequent
			 * samples go to NORMAL (single-fire guarantee). */
			float impact = st->impact_g;
			fall_reset(st);
			if (impact_g_out != NULL) {
				*impact_g_out = impact;
			}
			return true; /* confirmed fall */
		}
		if (st->post_count > post_max) {
			fall_reset(st); /* never settled -> not a fall */
		}
		break;
	}
	return false;
}
