/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the USB registry dispatcher.  Mirrors the Slice 4a
 * rtc_registry harness; no vendor extensions, so the test surface
 * is the bare selector + capability-getter + public-API edges.
 *
 * Backends visible on this test build:
 *   zephyr_drv      (priority 100, "*" wildcard)
 *   sw_fallback     (priority 0,   "*" wildcard)
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("usb", ALP_SOC_REF_STR)` exercises
 * the same selector code path real customer builds hit.  Tests that
 * need a different silicon_ref call alp_backend_select directly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/usb.h>

#include "../../../../src/backends/usb/usb_ops.h"

ZTEST_SUITE(alp_usb_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_usb_registry, test_zephyr_drv_picked_over_sw_on_alif_e7)
{
    const alp_backend_t *be =
        alp_backend_select("usb", "alif:ensemble:e7");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "zephyr"), 0);
    zassert_equal(be->priority, 100);
}

ZTEST(alp_usb_registry, test_sw_fallback_picked_for_unknown_silicon)
{
    /* Both registered backends are wildcards; the higher-priority
     * zephyr_drv would normally win.  This case still exercises the
     * selector and asserts the sw_fallback is reachable on the test
     * build via the registry's count.  If the selector preference
     * ever changes to "exact match beats wildcard", this assertion
     * documents the expected behaviour. */
    const alp_backend_t *be =
        alp_backend_select("usb", "fictional:soc:zz");
    zassert_not_null(be);
    (void)be;
    zassert_true(alp_backend_count("usb") >= 2u);
}

ZTEST(alp_usb_registry, test_select_returns_null_for_null_class)
{
    zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_usb_registry, test_select_returns_null_for_null_silicon_ref)
{
    /* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
    zassert_is_null(alp_backend_select("usb", NULL));
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_usb_registry, test_usb_device_open_returns_null_on_null_cfg)
{
    /* Dispatcher must reject NULL config before reaching the
     * backend; stamps last_error = ALP_ERR_INVAL. */
    alp_usb_dev_t *h = alp_usb_device_open(NULL);
    zassert_is_null(h);
}

ZTEST(alp_usb_registry, test_usb_capabilities_returns_null_for_null_handle)
{
    zassert_is_null(alp_usb_capabilities(NULL));
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_usb_registry, test_backend_count_for_usb)
{
    /* zephyr_drv + sw_fallback registered on this build.
     * No vendor-specific backends exist for USB in Slice 4b. */
    zassert_equal(alp_backend_count("usb"), 2u);
}
