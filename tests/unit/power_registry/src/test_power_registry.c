/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the Power registry dispatcher.  Mirrors the
 * Slice 5 camera_registry / Slice 4a rtc_registry harnesses.
 *
 * Backends visible on this test build (the prj.conf compiles in
 * the real Zephyr pm_policy backend on top of the wildcard stub):
 *   zephyr_pm_policy (priority 100, "*" wildcard, vendor "zephyr")
 *   zephyr_stub      (priority   0, "*" wildcard, vendor "stub"  )
 *
 * The pm_policy backend wins selection for every silicon_ref since
 * both are wildcards and 100 > 0.  The Renesas vendor-ext body
 * (src/backends/ext/renesas/power.c) is NOT linked in this build
 * (it depends on ALP_SDK_V2N_SUPERVISOR which depends on the
 * GD32G553 chip-driver Kconfig); the vendor-ext header is still
 * compile-tested via inclusion, and the NOT_PRESENT_ON_THIS_SOC
 * gate is exercised through the alif:ensemble:e7 silicon_ref.
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("power", ALP_SOC_REF_STR)`
 * exercises the same selector code path real customer builds hit.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/ext/renesas/power.h>
#include <alp/peripheral.h>
#include <alp/power.h>

#include "../../../../src/backends/power/power_ops.h"

ZTEST_SUITE(alp_power_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_power_registry, test_pm_policy_picked_for_alif_e7)
{
    /* Real Zephyr pm_policy backend (priority 100) wins over the
     * wildcard stub (priority 0) on every silicon. */
    const alp_backend_t *be =
        alp_backend_select("power", "alif:ensemble:e7");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "zephyr"), 0);
    zassert_equal(be->priority, 100);
}

ZTEST(alp_power_registry, test_pm_policy_picked_for_unknown_silicon)
{
    const alp_backend_t *be =
        alp_backend_select("power", "fictional:soc:zz");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "zephyr"), 0);
    zassert_equal(be->priority, 100);
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

ZTEST(alp_power_registry, test_power_open_and_close)
{
    /* The pm_policy backend's open() returns ALP_OK and a real
     * handle; close() releases the pm_policy lock-pool slot
     * cleanly.  Smoke-tests the open/close path without an actual
     * sleep (which would block native_sim indefinitely on K_FOREVER). */
    alp_power_t *h = alp_power_open();
    zassert_not_null(h);
    alp_power_close(h);
}

ZTEST(alp_power_registry, test_power_configure_wake_source)
{
    /* configure_wake_source mirrors the bitmap into the dispatcher
     * state without touching hardware on the pm_policy backend. */
    alp_power_t *h = alp_power_open();
    zassert_not_null(h);
    zassert_equal(alp_power_configure_wake_source(h, ALP_POWER_WAKE_RTC),
                  ALP_OK);
    zassert_equal(alp_power_configure_wake_source(h,
                      ALP_POWER_WAKE_RTC | ALP_POWER_WAKE_GPIO),
                  ALP_OK);
    /* The empty bitmap is legal at the configure call -- the INVAL
     * guard fires only at request_sleep() time. */
    zassert_equal(alp_power_configure_wake_source(h, ALP_POWER_WAKE_NONE),
                  ALP_OK);
    alp_power_close(h);
}

ZTEST(alp_power_registry, test_power_request_sleep_inval_guards)
{
    /* Exercise the dispatcher's INVAL pre-checks against a real
     * handle:
     *   - RUN mode is rejected outright.
     *   - Invalid enum (cast from out-of-range int) likewise.
     *   - No wake source AND zero wake_after_ms is rejected too.
     * All three return BEFORE the backend's request_sleep op runs
     * (which would otherwise block on the wake semaphore). */
    alp_power_t *h = alp_power_open();
    zassert_not_null(h);

    zassert_equal(alp_power_request_sleep(h, ALP_POWER_MODE_RUN, 1000u, NULL),
                  ALP_ERR_INVAL);
    zassert_equal(alp_power_request_sleep(h, (alp_power_mode_t)99, 1000u, NULL),
                  ALP_ERR_INVAL);
    zassert_equal(alp_power_request_sleep(h, ALP_POWER_MODE_SLEEP, 0u, NULL),
                  ALP_ERR_INVAL);

    alp_power_close(h);
}

ZTEST(alp_power_registry, test_power_request_sleep_with_timer_wakes)
{
    /* End-to-end exercise of the pm_policy backend's request_sleep:
     * non-zero wake_after_ms registers the k_timer wake, the call
     * parks on the semaphore, the timer fires, the locks rebalance
     * around the descent + ascent, and the call returns ALP_OK with
     * info filled.  Uses a tiny 10 ms wake so the test stays fast. */
    alp_power_t *h = alp_power_open();
    zassert_not_null(h);
    zassert_equal(alp_power_configure_wake_source(h, ALP_POWER_WAKE_RTC),
                  ALP_OK);

    alp_power_wake_info_t info = { 0 };
    alp_status_t          rc =
        alp_power_request_sleep(h, ALP_POWER_MODE_SLEEP, 10u, &info);
    zassert_equal(rc, ALP_OK);
    zassert_equal(info.realised_mode, ALP_POWER_MODE_SLEEP);
    zassert_equal(info.wake_source, (uint32_t)ALP_POWER_WAKE_RTC);
    /* slept_ms is best-effort; assert >= 0 (i.e. just non-negative
     * which is implicit in the uint32_t type) -- the exact value
     * depends on the scheduler tick + timer granularity. */

    alp_power_close(h);
}

ZTEST(alp_power_registry, test_power_capabilities_returns_null_for_null_handle)
{
    zassert_is_null(alp_power_capabilities(NULL));
}

/* ---------- Vendor-ext bypass test --------------------------------- */

ZTEST(alp_power_registry, test_renesas_vendor_ext_returns_inval_on_null_handle)
{
    /* NULL-handle gate fires before any backend dispatch. */
    zassert_equal(alp_renesas_power_supervisor_mode_set(NULL, 1u),
                  ALP_ERR_INVAL);
}

ZTEST(alp_power_registry, test_renesas_vendor_ext_rejects_non_renesas_backend)
{
    /* The active backend in this test build is the Zephyr pm_policy
     * backend (vendor "zephyr"); the vendor-ext must reject the
     * handle with NOT_PRESENT_ON_THIS_SOC BEFORE reaching for the
     * supervisor.  This is the "bypass to NOSUPPORT-but-validation-
     * passed" path the vendor-ext audit rule requires. */
    alp_power_t *h = alp_power_open();
    zassert_not_null(h);

    zassert_equal(alp_renesas_power_supervisor_mode_set(h, 1u),
                  ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
    zassert_equal(alp_renesas_power_supervisor_mode_set(h, 0u),
                  ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
    /* Higher mode values are also rejected by the same gate -- the
     * gate runs before any payload validation. */
    zassert_equal(alp_renesas_power_supervisor_mode_set(h, 99u),
                  ALP_ERR_NOT_PRESENT_ON_THIS_SOC);

    alp_power_close(h);
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_power_registry, test_backend_count_for_power)
{
    /* Two backends registered on this build: the wildcard stub and
     * the real pm_policy backend.  The Renesas vendor-ext is NOT a
     * power backend (it shadows the public alp_power_t handle but
     * doesn't register into the registry); it earns no slot here. */
    zassert_equal(alp_backend_count("power"), 2u);
}
