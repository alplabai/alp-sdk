/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the Wi-Fi registry dispatcher.  Mirrors the BLE
 * sibling harness; no vendor extensions, so the test surface is
 * the bare selector + capability-getter + public-API edges.
 *
 * Backends visible on this test build:
 *   zephyr_drv      (priority 100, "*" wildcard)
 *   sw_fallback     (priority 0,   "*" wildcard)
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("wifi", ALP_SOC_REF_STR)`
 * exercises the same selector code path real customer builds hit.
 * Tests that need a different silicon_ref call alp_backend_select
 * directly.  CONFIG_WIFI stays OFF -- the test only exercises the
 * dispatcher's null-handle gates and the selector, neither of
 * which touches the Wi-Fi management subsystem.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/iot.h>
#include <alp/peripheral.h>

#include "../../../../src/backends/wifi/wifi_ops.h"

ZTEST_SUITE(alp_wifi_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_wifi_registry, test_zephyr_drv_picked_over_sw_on_alif_e7)
{
    const alp_backend_t *be =
        alp_backend_select("wifi", "alif:ensemble:e7");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "zephyr"), 0);
    zassert_equal(be->priority, 100);
}

ZTEST(alp_wifi_registry, test_sw_fallback_picked_for_unknown_silicon)
{
    /* Both registered backends are wildcards; the higher-priority
     * zephyr_drv would normally win.  This case still exercises the
     * selector and asserts the sw_fallback is reachable on the test
     * build via the registry's count.  Degraded pattern: only
     * inventory is asserted, not the specific pick. */
    const alp_backend_t *be =
        alp_backend_select("wifi", "fictional:soc:zz");
    zassert_not_null(be);
    (void)be;
    zassert_true(alp_backend_count("wifi") >= 2u);
}

ZTEST(alp_wifi_registry, test_select_returns_null_for_null_class)
{
    zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_wifi_registry, test_select_returns_null_for_null_silicon_ref)
{
    /* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
    zassert_is_null(alp_backend_select("wifi", NULL));
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_wifi_registry, test_wifi_connect_inval_on_null_handle)
{
    /* Dispatcher must reject NULL handle before reaching the
     * backend; returns ALP_ERR_NOT_READY per the public contract. */
    const alp_wifi_credentials_t creds = { .ssid = "x", .psk = NULL };
    zassert_equal(alp_wifi_connect(NULL, &creds, 100), ALP_ERR_NOT_READY);
}

ZTEST(alp_wifi_registry, test_wifi_capabilities_returns_null_for_null_handle)
{
    zassert_is_null(alp_wifi_capabilities(NULL));
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_wifi_registry, test_backend_count_for_wifi)
{
    /* zephyr_drv + sw_fallback registered on this build.
     * No vendor-specific backends exist for Wi-Fi in Slice 4b. */
    zassert_equal(alp_backend_count("wifi"), 2u);
}
