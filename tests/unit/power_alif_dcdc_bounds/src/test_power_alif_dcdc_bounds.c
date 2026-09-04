/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #1626: white-box native_sim coverage that the Alif SE DC-DC
 * rail-voltage guard actually gates the SHIPPED call sites inside
 * alif_profile_set() (both the RUN and STANDBY halves) -- not just the
 * pure bound helper in isolation.
 *
 * A prior version of this regression suite (tests/unit/power_registry/
 * src/test_power_registry.c's test_alif_dcdc_mv_bounds) called
 * alp_power_alif_dcdc_mv_valid() directly.  Reverting BOTH guard call
 * sites in alif_profile_set() left that suite green -- it only proved the
 * standalone helper works, never that alif_profile_set() actually calls
 * it.  This suite instead compiles the REAL src/backends/power/
 * alif_se_profile.c translation unit (via the #include below -- the same
 * technique tests/unit/se_cryptocell_hash_bounds/src/test_se_hash_bounds.c
 * uses for the equivalent Alif-SE-gated situation) and calls the SHIPPED
 * alif_profile_set() through the backend's own vtable (`_ops`, file-scope
 * `static` in alif_se_profile.c and therefore visible here after the
 * #include).
 *
 * alif_se_profile.c is gated behind CONFIG_ALP_SDK_POWER_PROFILE_ALIF_SE,
 * which `depends on` the AEN801/E8-only hal_alif Kconfig symbol
 * HAS_ALIF_SE_SERVICES and so can never be selected on native_sim through
 * the real Kconfig/zephyr_library path -- this test instead fakes the
 * macro directly at the preprocessor level (mirrors
 * test_se_hash_bounds.c's CONFIG_ALP_SDK_SECURITY_SE_CRYPTOCELL fake).
 *
 * The fake se_service_set_run_cfg() / se_service_set_off_cfg() below bump
 * a counter instead of doing anything -- the assertion that the counter
 * stayed 0 across an out-of-range rail_mv is the proof the SE mailbox was
 * never written with the bad value, not just that alif_profile_set()
 * happened to return an error code.
 */

/* Faked purely at the preprocessor level -- no real
 * ALP_SDK_POWER_PROFILE_ALIF_SE Kconfig symbol exists in this image (see
 * this directory's CMakeLists.txt for why CONFIG_ALP_SDK is deliberately
 * never set here). */
#define CONFIG_ALP_SDK_POWER_PROFILE_ALIF_SE 1

#include "../../../../src/backends/power/alif_se_profile.c"

#include <stdint.h>

#include <zephyr/ztest.h>

/* ------------------------------------------------------------------ */
/* Fake hal_alif SE-service getters/setters                            */
/* ------------------------------------------------------------------ */

static size_t g_run_cfg_set_calls;
static size_t g_off_cfg_set_calls;

int se_service_get_run_cfg(run_profile_t *pp)
{
	*pp = (run_profile_t){ 0 };
	return 0;
}

int se_service_set_run_cfg(run_profile_t *pp)
{
	(void)pp;
	g_run_cfg_set_calls++;
	return 0;
}

int se_service_get_off_cfg(off_profile_t *wp)
{
	*wp = (off_profile_t){ 0 };
	return 0;
}

int se_service_set_off_cfg(off_profile_t *wp)
{
	(void)wp;
	g_off_cfg_set_calls++;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Suite                                                               */
/* ------------------------------------------------------------------ */

static void reset_state(void *fixture)
{
	(void)fixture;
	g_run_cfg_set_calls = 0u;
	g_off_cfg_set_calls = 0u;
}

ZTEST_SUITE(power_alif_dcdc_bounds, NULL, NULL, reset_state, NULL, NULL);

ZTEST(power_alif_dcdc_bounds, test_run_rejects_uv_typo_without_touching_se)
{
	/* The issue's mV/uV typo repro: 8000 "mV" (actually 8 V) must be
	 * rejected before se_service_set_run_cfg() is ever called. */
	const alp_power_profile_t p = { .rail_mv = 8000u };

	zassert_equal(_ops.set(ALP_POWER_PROFILE_RUN, &p), ALP_ERR_INVAL);
	zassert_equal(g_run_cfg_set_calls, 0u, "SE mailbox must never see the bad value");
}

ZTEST(power_alif_dcdc_bounds, test_run_rejects_one_below_min_without_touching_se)
{
	const alp_power_profile_t p = { .rail_mv = 749u };

	zassert_equal(_ops.set(ALP_POWER_PROFILE_RUN, &p), ALP_ERR_INVAL);
	zassert_equal(g_run_cfg_set_calls, 0u);
}

ZTEST(power_alif_dcdc_bounds, test_run_accepts_boundary_values)
{
	const alp_power_profile_t lo = { .rail_mv = 750u };
	const alp_power_profile_t hi = { .rail_mv = 850u };

	zassert_equal(_ops.set(ALP_POWER_PROFILE_RUN, &lo), ALP_OK);
	zassert_equal(_ops.set(ALP_POWER_PROFILE_RUN, &hi), ALP_OK);
	zassert_equal(g_run_cfg_set_calls, 2u, "both in-range writes must reach the SE");
}

ZTEST(power_alif_dcdc_bounds, test_standby_rejects_uv_typo_without_touching_se)
{
	const alp_power_profile_t p = { .rail_mv = 8000u };

	zassert_equal(_ops.set(ALP_POWER_PROFILE_STANDBY, &p), ALP_ERR_INVAL);
	zassert_equal(g_off_cfg_set_calls, 0u, "SE mailbox must never see the bad value");
}

ZTEST(power_alif_dcdc_bounds, test_standby_rejects_one_above_max_without_touching_se)
{
	const alp_power_profile_t p = { .rail_mv = 851u };

	zassert_equal(_ops.set(ALP_POWER_PROFILE_STANDBY, &p), ALP_ERR_INVAL);
	zassert_equal(g_off_cfg_set_calls, 0u);
}

ZTEST(power_alif_dcdc_bounds, test_standby_accepts_boundary_values)
{
	const alp_power_profile_t lo = { .rail_mv = 750u };
	const alp_power_profile_t hi = { .rail_mv = 850u };

	zassert_equal(_ops.set(ALP_POWER_PROFILE_STANDBY, &lo), ALP_OK);
	zassert_equal(_ops.set(ALP_POWER_PROFILE_STANDBY, &hi), ALP_OK);
	zassert_equal(g_off_cfg_set_calls, 2u, "both in-range writes must reach the SE");
}
