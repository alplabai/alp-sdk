/* src/vision/arms.c -- see arms.h. */
#include "arms.h"

enum { ARM_DOWN = 0, ARM_RISING, ARM_SPENT };

void tr_arms_reset(tr_arms_t *a)
{
	*a = (tr_arms_t){ 0 };
}

tr_arm_event_t tr_arms_step(tr_arms_t *a, const int16_t raise[2])
{
	tr_arm_event_t ev      = { 0 };
	bool           rose[2] = { false, false }; /* crossed the raise line on THIS pose */

	for (int k = 0; k < 2; k++) {
		if (a->state[k] == ARM_RISING && a->age[k] < UINT8_MAX) {
			a->age[k]++; /* time since the rise: every new pose, judged or not */
		}
		if (raise[k] == TR_ARM_UNKNOWN) {
			continue; /* not judged: the state stays, and does not prime the arm */
		}
		bool first = !a->primed[k];

		a->primed[k] = true;
		if (raise[k] >= TR_ARM_UP_PCT) {
			if (a->state[k] == ARM_DOWN) {
				/* Already up when first judged (a joiner with a hand raised): spent. */
				a->state[k] = first ? ARM_SPENT : ARM_RISING;
				a->age[k]   = 1u;
				rose[k]     = !first;
			}
		} else if (raise[k] <= TR_ARM_DOWN_PCT) {
			a->state[k] = ARM_DOWN;
			a->age[k]   = 0u;
		}
		/* between the two thresholds: hysteresis, the state stays */
	}

	if (a->state[TR_ARM_LEFT] == ARM_RISING && a->state[TR_ARM_RIGHT] == ARM_RISING) {
		/* Both rose inside the window: a jump, and neither arm may also step a lane. */
		ev.jump                = true;
		a->state[TR_ARM_LEFT]  = ARM_SPENT;
		a->state[TR_ARM_RIGHT] = ARM_SPENT;
		return ev;
	}
	for (int k = 0; k < 2; k++) {
		int o = 1 - k;

		if (rose[k] && a->state[o] == ARM_SPENT && raise[o] != TR_ARM_UNKNOWN &&
		    raise[o] >= TR_ARM_UP_PCT) {
			/* The other arm is still up from its own step: a late both-arms raise. */
			ev.jump     = true;
			a->state[k] = ARM_SPENT;
			return ev;
		}
	}
	for (int k = 0; k < 2; k++) {
		if (a->state[k] == ARM_RISING && a->age[k] >= TR_ARM_SETTLE_POSES) {
			if (raise[k] != TR_ARM_UNKNOWN && raise[k] >= TR_ARM_UP_PCT) {
				ev.lane_delta = k == TR_ARM_LEFT ? -1 : +1;
				a->state[k]   = ARM_SPENT;
			} else {
				a->state[k] = ARM_DOWN; /* never confirmed: out of frame or fell back */
				a->age[k]   = 0u;
			}
		}
	}
	return ev;
}
