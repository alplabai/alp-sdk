/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the Display registry dispatcher.  Mirrors the
 * Slice 5 camera_registry / Slice 7 power_registry harnesses.  No
 * vendor extensions exist for display in Slice 8a, so the test
 * surface is the bare selector + capability-getter + public-API
 * edges.
 *
 * Backends visible on this test build:
 *   zephyr_stub     (priority 0, "*" wildcard, vendor "stub")
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("display", ALP_SOC_REF_STR)`
 * exercises the same selector code path real customer builds hit.
 * Tests that need a different silicon_ref call alp_backend_select
 * directly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/display.h>
#include <alp/peripheral.h>

#include "../../../../src/backends/display/display_ops.h"

ZTEST_SUITE(alp_display_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_display_registry, test_stub_picked_for_alif_e7)
{
	/* The wildcard stub is the only display backend registered in
     * Slice 8a; any silicon_ref resolves to it. */
	const alp_backend_t *be = alp_backend_select("display", "alif:ensemble:e7");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "stub"), 0);
	zassert_equal(be->priority, 0);
}

ZTEST(alp_display_registry, test_stub_picked_for_unknown_silicon)
{
	const alp_backend_t *be = alp_backend_select("display", "fictional:soc:zz");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "stub"), 0);
	zassert_equal(be->priority, 0);
}

ZTEST(alp_display_registry, test_select_returns_null_for_null_class)
{
	zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_display_registry, test_select_returns_null_for_null_silicon_ref)
{
	/* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
	zassert_is_null(alp_backend_select("display", NULL));
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_display_registry, test_display_open_returns_null_on_null_cfg)
{
	/* Dispatcher rejects NULL config at the front door before any
     * backend gets called. */
	alp_display_t *h = alp_display_open(NULL);
	zassert_is_null(h);
}

ZTEST(alp_display_registry, test_display_capabilities_returns_null_for_null_handle)
{
	zassert_is_null(alp_display_capabilities(NULL));
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_display_registry, test_backend_count_for_display)
{
	/* Only zephyr_stub registered on this build -- no vendor-specific
     * display backends exist in Slice 8a. */
	zassert_equal(alp_backend_count("display"), 1u);
}
