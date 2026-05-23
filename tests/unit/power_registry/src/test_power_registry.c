/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the Power registry dispatcher.  Mirrors the
 * Slice 5 camera_registry / Slice 4a rtc_registry harnesses.  No
 * vendor extensions exist for power in Slice 7, so the test surface
 * is the bare selector + capability-getter + public-API edges.
 *
 * Backends visible on this test build:
 *   zephyr_stub     (priority 0, "*" wildcard, vendor "stub")
 *
 * The stub's open() returns ALP_OK so alp_power_open() hands back a
 * real handle on this build; the request_sleep INVAL gates are
 * exercised against that real handle.
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("power", ALP_SOC_REF_STR)`
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
#include <alp/peripheral.h>
#include <alp/power.h>

#include "../../../../src/backends/power/power_ops.h"

ZTEST_SUITE(alp_power_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_power_registry, test_stub_picked_for_alif_e7)
{
    /* The wildcard stub is the only power backend registered in
     * Slice 7; any silicon_ref resolves to it. */
    const alp_backend_t *be =
        alp_backend_select("power", "alif:ensemble:e7");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "stub"), 0);
    zassert_equal(be->priority, 0);
}

ZTEST(alp_power_registry, test_stub_picked_for_unknown_silicon)
{
    const alp_backend_t *be =
        alp_backend_select("power", "fictional:soc:zz");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "stub"), 0);
    zassert_equal(be->priority, 0);
}

ZTEST(alp_power_registry, test_select_returns_null_for_null_class)
{
    zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_power_registry, test_select_returns_null_for_null_silicon_ref)
{
    /* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
    zassert_is_null(alp_backend_select("power", NULL));
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_power_registry, test_power_request_sleep_inval_on_run_mode)
{
    /* The stub's open() returns ALP_OK so we get a real handle;
     * exercise the dispatcher's INVAL pre-checks against it.
     *
     *   - RUN mode is rejected outright.
     *   - Invalid enum (cast from out-of-range int) likewise.
     *   - No wake source AND zero wake_after_ms is rejected too. */
    alp_power_t *h = alp_power_open();
    zassert_not_null(h);

    zassert_equal(alp_power_request_sleep(h, ALP_POWER_MODE_RUN, 1000u, NULL),
                  ALP_ERR_INVAL);
    zassert_equal(alp_power_request_sleep(h, (alp_power_mode_t)99, 1000u, NULL),
                  ALP_ERR_INVAL);
    zassert_equal(alp_power_request_sleep(h, ALP_POWER_MODE_SLEEP, 0u, NULL),
                  ALP_ERR_INVAL);

    /* With a configured wake source the INVAL guard releases and the
     * stub backend's NOT_IMPLEMENTED surfaces through. */
    zassert_equal(alp_power_configure_wake_source(h, ALP_POWER_WAKE_RTC),
                  ALP_OK);
    zassert_equal(alp_power_request_sleep(h, ALP_POWER_MODE_SLEEP, 0u, NULL),
                  ALP_ERR_NOT_IMPLEMENTED);

    alp_power_close(h);
}

ZTEST(alp_power_registry, test_power_capabilities_returns_null_for_null_handle)
{
    zassert_is_null(alp_power_capabilities(NULL));
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_power_registry, test_backend_count_for_power)
{
    /* Only zephyr_stub registered on this build -- no vendor-specific
     * power backends exist in Slice 7. */
    zassert_equal(alp_backend_count("power"), 1u);
}
