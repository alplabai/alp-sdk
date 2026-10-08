/* src/vision/arms.c -- see arms.h. */
#include "arms.h"

enum { ARM_DOWN = 0, ARM_RISING, ARM_SPENT };

void tr_arms_reset(tr_arms_t *a)
{
	*a = (tr_arms_t){ 0 };
}

tr_arm_event_t tr_arms_step(tr_arms_t *a, const int16_t raise[2])
{
	tr_arm_event_t ev = { 0 };

	for (int k = 0; k < 2; k++) {
		if (raise[k] == TR_ARM_UNKNOWN) {
			continue; /* not judged: keep the state, do not advance the age */
		}
		if (raise[k] >= TR_ARM_UP_PCT) {
			if (a->state[k] == ARM_DOWN) {
				/* Already up on the very first pose (a joiner with a hand raised): spent. */
				a->state[k] = a->primed ? ARM_RISING : ARM_SPENT;
				a->age[k]   = 1u;
			} else if (a->state[k] == ARM_RISING && a->age[k] < UINT8_MAX) {
				a->age[k]++;
			}
		} else if (raise[k] <= TR_ARM_DOWN_PCT) {
			a->state[k] = ARM_DOWN;
			a->age[k]   = 0u;
		}
		/* between the two thresholds: hysteresis, nothing changes */
	}
	a->primed = true;

	if (a->state[TR_ARM_LEFT] == ARM_RISING && a->state[TR_ARM_RIGHT] == ARM_RISING) {
		/* Both rose inside the window (a lone arm would have fired by now): a jump, and
		 * neither arm may also step a lane. */
		ev.jump                = true;
		a->state[TR_ARM_LEFT]  = ARM_SPENT;
		a->state[TR_ARM_RIGHT] = ARM_SPENT;
		return ev;
	}
	for (int k = 0; k < 2; k++) {
		if (a->state[k] == ARM_RISING && a->age[k] >= TR_ARM_SETTLE_POSES) {
			ev.lane_delta = k == TR_ARM_LEFT ? -1 : +1;
			a->state[k]   = ARM_SPENT;
		}
	}
	return ev;
}
