/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #2037: examples/aen/aen-evk-demo's Phase 11 (sound out -> PDM in)
 * correlation verdict, pulled into its own header precisely so it can be
 * exercised here without a board -- see sound_verdict.h's file comment for
 * the energy-check reasoning and what it deliberately does not claim.
 * Reachable via this app's existing examples/aen/aen-evk-demo/src include
 * dir (see bmp581_verdict.h's comment in this app's CMakeLists.txt).
 */

#include <zephyr/ztest.h>

#include "sound_verdict.h"

ZTEST(alp_chips, test_sound_correlated_when_during_clears_ratio_and_floor)
{
	/* A quiet-room baseline and a during-playback sum well past both the
	 * 2x ratio and the absolute floor -- the shape a genuine played tone
	 * produces. */
	zassert_true(sound_pdm_capture_correlated(1000u, 4000u),
	             "4x baseline, well past the floor, must be correlated");
}

ZTEST(alp_chips, test_sound_not_correlated_when_during_matches_baseline)
{
	/* Same energy before and during -- nothing the mic heard changed when
	 * the tone started, which is exactly what "the loop is broken" looks
	 * like (dead amp, dead mic, or a mux routed the wrong way). */
	zassert_false(sound_pdm_capture_correlated(2000u, 2000u),
	              "equal energy before/during must NOT be correlated");
}

ZTEST(alp_chips, test_sound_floor_refuses_near_zero_baseline)
{
	/* baseline_energy = 0 makes the ratio side trivially satisfied by
	 * ANY nonzero during_energy -- the floor is what stops a capture
	 * that stayed silent throughout from passing on that technicality. */
	zassert_false(sound_pdm_capture_correlated(0u, SOUND_ENERGY_FLOOR - 1u),
	              "a during_energy under the floor must fail even against a zero baseline");
	zassert_true(sound_pdm_capture_correlated(0u, SOUND_ENERGY_FLOOR),
	             "a during_energy at the floor, against a zero baseline, must pass");
}

ZTEST(alp_chips, test_sound_ratio_boundary_is_inclusive)
{
	/* Exactly 2x baseline, at the ratio this header defines -- the
	 * boundary itself must pass, and one unit short must not. Pins the
	 * cross-multiply against a divide-based rewrite rounding the wrong
	 * way at the boundary. */
	zassert_true(sound_pdm_capture_correlated(1000u, 2000u), "exactly 2x baseline must pass");
	zassert_false(sound_pdm_capture_correlated(1000u, 1999u), "one unit under 2x must not pass");
}
