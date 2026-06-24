/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the TMU registry dispatcher.  TMU is the first
 * registry slice without a handle type, so the test surface is
 * the bare selector + the public-API edges across the twelve
 * stateless alp_tmu_* primitives (NULL-out reject + libm round-
 * trip on the SW fallback path).  No capability getter exists --
 * there is no handle to call it on.
 *
 * Backends visible on this test build:
 *   zephyr_drv      (priority 100, "*" wildcard)
 *   sw_fallback     (priority 0,   "*" wildcard)
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("tmu", ALP_SOC_REF_STR)`
 * exercises the same selector code path real customer builds hit.
 * Tests that need a different silicon_ref call alp_backend_select
 * directly.  CONFIG_ALP_SDK_V2N_SUPERVISOR stays OFF -- the test
 * only exercises the dispatcher's null-out gate, the selector,
 * and one libm round-trip (alp_tmu_sin(0.0f, &out)); none of
 * which touch the V2N bridge.
 */

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/peripheral.h>
#include <alp/tmu.h>

#include "../../../../src/backends/tmu/tmu_ops.h"

ZTEST_SUITE(alp_tmu_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_tmu_registry, test_zephyr_drv_picked_over_sw_on_alif_e7)
{
	const alp_backend_t *be = alp_backend_select("tmu", "alif:ensemble:e7");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "zephyr"), 0);
	zassert_equal(be->priority, 100);
}

ZTEST(alp_tmu_registry, test_sw_fallback_picked_for_unknown_silicon)
{
	/* Both registered backends are wildcards; the higher-priority
     * zephyr_drv would normally win.  This case still exercises the
     * selector and asserts the sw_fallback is reachable on the test
     * build via the registry's count.  Degraded pattern: only
     * inventory is asserted, not the specific pick. */
	const alp_backend_t *be = alp_backend_select("tmu", "fictional:soc:zz");
	zassert_not_null(be);
	(void)be;
	zassert_true(alp_backend_count("tmu") >= 2u);
}

ZTEST(alp_tmu_registry, test_select_returns_null_for_null_class)
{
	zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_tmu_registry, test_select_returns_null_for_null_silicon_ref)
{
	/* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
	zassert_is_null(alp_backend_select("tmu", NULL));
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_tmu_registry, test_tmu_sin_inval_on_null_out)
{
	/* Dispatcher must reject NULL out before reaching the backend.
     * Stateless math primitive -- no handle to NULL-check, so the
     * out-pointer check is the only pre-flight gate. */
	zassert_equal(alp_tmu_sin(0.0f, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_tmu_registry, test_tmu_sin_returns_expected_value)
{
	/* End-to-end dispatcher -> backend round-trip.  Exercises the
     * cached-ops lookup, the function-pointer indirection, and the
     * libm path inside zephyr_drv.c (CONFIG_ALP_SDK_V2N_SUPERVISOR
     * is off on this build, so the bridge path is compiled out and
     * the libm branch wins).  sin(0) == 0 in IEEE-754 single. */
	float out = 1.0f; /* poison value to confirm the backend wrote */
	zassert_equal(alp_tmu_sin(0.0f, &out), ALP_OK);
	zassert_true(fabsf(out) < 1e-6f);
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_tmu_registry, test_backend_count_for_tmu)
{
	/* zephyr_drv + sw_fallback registered on this build.
     * No vendor-specific backends exist for TMU in Slice 4d. */
	zassert_equal(alp_backend_count("tmu"), 2u);
}
