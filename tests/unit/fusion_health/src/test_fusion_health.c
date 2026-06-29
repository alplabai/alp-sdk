/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for fusion_health (cross-modal sensor fusion) -- native_sim.
 */
#include <string.h>
#include <zephyr/ztest.h>
#include "fusion_health.h"

ZTEST_SUITE(fusion_health, NULL, NULL, NULL, NULL, NULL);

/* A small-motor baseline: nominal + tolerance per fusion_input field, in field
 * order {vib_rms, vib_crest, current_a, current_ripple, temp_c, temp_slope}. */
static const struct fusion_baseline BASE = {
	.nominal = { 0.05f, 3.0f, 1.0f, 0.05f, 30.0f, 0.0f },
	.tol     = { 0.05f, 2.0f, 0.5f, 0.05f, 10.0f, 0.5f },
};

/* Start from the healthy nominal, then the tests perturb individual fields by
 * 3x tolerance to drive a modality's sub-score to 3.0 (clearly > 1.0). */
static struct fusion_input nominal_input(void)
{
	struct fusion_input in = { .vib_rms        = 0.05f,
		                       .vib_crest      = 3.0f,
		                       .current_a      = 1.0f,
		                       .current_ripple = 0.05f,
		                       .temp_c         = 30.0f,
		                       .temp_slope     = 0.0f };
	return in;
}

ZTEST(fusion_health, test_healthy)
{
	struct fusion_input  in = nominal_input();
	struct fusion_result r;
	fusion_assess(&in, &BASE, &r);
	zassert_equal(r.hypothesis, FUSION_HEALTHY, "all nominal -> HEALTHY");
	zassert_equal(r.corroboration, 0, "no modality anomalous");
	zassert_true(r.health_score < 0.05f, "healthy -> ~0 health score");
}

ZTEST(fusion_health, test_bearing_wear)
{
	struct fusion_input in = nominal_input();
	in.vib_rms             = 0.05f + 3.0f * 0.05f; /* +3 tol -> vib_score ~3 */
	in.temp_c              = 30.0f + 3.0f * 10.0f; /* +3 tol -> temp_score ~3 */
	struct fusion_result r;
	fusion_assess(&in, &BASE, &r);
	zassert_equal(r.hypothesis, FUSION_BEARING_WEAR, "vibration + heat -> BEARING_WEAR");
	zassert_equal(r.corroboration, 2, "two modalities corroborate");
	zassert_true(r.health_score > 0.5f, "corroborated severe fault -> high health score");
}

ZTEST(fusion_health, test_electrical_fault)
{
	struct fusion_input in = nominal_input();
	in.current_a           = 1.0f + 3.0f * 0.5f; /* current over tolerance, others nominal */
	struct fusion_result r;
	fusion_assess(&in, &BASE, &r);
	zassert_equal(r.hypothesis, FUSION_ELECTRICAL_FAULT, "current-only -> ELECTRICAL_FAULT");
	zassert_equal(r.corroboration, 1, "one modality anomalous");
}

ZTEST(fusion_health, test_mechanical_overload)
{
	struct fusion_input in = nominal_input();
	in.vib_rms             = 0.05f + 3.0f * 0.05f;
	in.current_a           = 1.0f + 3.0f * 0.5f;
	in.temp_c              = 30.0f + 3.0f * 10.0f;
	struct fusion_result r;
	fusion_assess(&in, &BASE, &r);
	zassert_equal(r.hypothesis, FUSION_MECHANICAL_OVERLOAD, "all three -> MECHANICAL_OVERLOAD");
	zassert_equal(r.corroboration, 3, "all three corroborate");
}

ZTEST(fusion_health, test_uncorroborated_is_low_confidence)
{
	struct fusion_input in = nominal_input();
	in.vib_rms             = 0.05f + 3.0f * 0.05f; /* vibration alone, no heat/current */
	struct fusion_result r;
	fusion_assess(&in, &BASE, &r);
	zassert_equal(r.hypothesis, FUSION_UNCORROBORATED, "single modality -> UNCORROBORATED");
	zassert_equal(r.corroboration, 1, "one modality anomalous");
	/* Same severity as the bearing case (sub-score ~3) but discounted by the
	 * 0.5 confidence factor, so it must score lower than a corroborated fault. */
	zassert_true(r.health_score < 0.6f, "uncorroborated is discounted below a corroborated fault");
}

ZTEST(fusion_health, test_pack_dim_and_names)
{
	struct fusion_input  in = nominal_input();
	struct fusion_result r;
	float                vec[FUSION_FEATURE_DIM];
	fusion_assess(&in, &BASE, &r);
	zassert_equal(fusion_pack(&r, &in, vec, FUSION_FEATURE_DIM),
	              (size_t)FUSION_FEATURE_DIM,
	              "pack writes FUSION_FEATURE_DIM");
	zassert_true(strcmp(fusion_fault_name(FUSION_BEARING_WEAR), "BEARING_WEAR") == 0, "name");
	zassert_true(strcmp(fusion_fault_name(FUSION_HEALTHY), "HEALTHY") == 0, "name");
}
