/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for fall_detect (3-phase fall state machine) -- native_sim.
 */
#include <math.h>
#include <zephyr/ztest.h>
#include "fall_detect.h"

ZTEST_SUITE(fall_detect, NULL, NULL, NULL, NULL, NULL);

#define SR 100.0f

/* Push a run of `count` samples at constant magnitude; returns true if a fall
 * fired during the run, capturing impact_g. */
static bool push_const(struct fall_state *st, float g, int count, float *impact)
{
	bool fired = false;
	for (int i = 0; i < count; i++) {
		float ig = 0.0f;
		if (fall_push(st, g, SR, &ig)) {
			fired   = true;
			*impact = ig;
		}
	}
	return fired;
}

ZTEST(fall_detect, test_canonical_fall_fires)
{
	struct fall_state st;
	fall_reset(&st);
	float impact = 0.0f;

	push_const(&st, 1.0f, 30, &impact);                      /* normal */
	push_const(&st, 0.2f, 15, &impact);                      /* free-fall (>= 8 samples) */
	bool fired_impact = push_const(&st, 5.0f, 2, &impact);   /* impact spike */
	bool fired_still  = push_const(&st, 1.0f, 120, &impact); /* >=1 s stillness */

	zassert_true(fired_impact || fired_still, "a 3-phase fall is detected");
	zassert_within((double)impact, 5.0, 0.5, "impact_g captured ~5 g");
}

ZTEST(fall_detect, test_walk_never_fires)
{
	struct fall_state st;
	fall_reset(&st);
	float impact = 0.0f;
	bool  fired  = false;
	for (int i = 0; i < 1000; i++) {
		float g  = 1.0f + 0.6f * sinf(2.0f * (float)M_PI * 2.0f * (float)i / SR);
		float ig = 0.0f;
		if (fall_push(&st, g, SR, &ig)) {
			fired = true;
		}
		(void)impact;
	}
	zassert_false(fired, "ordinary walking never triggers a fall");
}

ZTEST(fall_detect, test_impact_without_freefall_does_not_fire)
{
	struct fall_state st;
	fall_reset(&st);
	float impact = 0.0f;
	/* Hard sit-down: no free-fall, just a 3 g bump then settle. */
	push_const(&st, 1.0f, 30, &impact);
	bool a = push_const(&st, 3.0f, 2, &impact);
	bool b = push_const(&st, 1.0f, 120, &impact);
	zassert_false(a || b, "impact with no preceding free-fall is not a fall");
}

ZTEST(fall_detect, test_freefall_without_impact_does_not_fire)
{
	struct fall_state st;
	fall_reset(&st);
	float impact = 0.0f;
	push_const(&st, 1.0f, 30, &impact);
	push_const(&st, 0.2f, 15, &impact);               /* free-fall ... */
	bool fired = push_const(&st, 1.0f, 200, &impact); /* ... then normal, no impact */
	zassert_false(fired, "free-fall with no impact is not a fall");
}
