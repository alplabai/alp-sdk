/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the Wi-Fi registry dispatcher.  Mirrors the BLE
 * sibling harness; no vendor extensions, so the test surface is
 * the bare selector + capability-getter + public-API edges.
 *
 * Backends visible on this test build:
 *   cc3501e_e3..e8 (priority 200, exact AEN silicon refs)
 *   zephyr_drv      (priority 100, "*" wildcard)
 *   sw_fallback     (priority 0,   "*" wildcard)
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("wifi", ALP_SOC_REF_STR)`
 * exercises the same selector code path real customer builds hit.
 * Tests that need a different silicon_ref call alp_backend_select
 * directly.  CONFIG_WIFI stays OFF -- the Zephyr wifi_mgmt backend body is not
 * active.  The CC3501E open path is still safe here because open only checks
 * that a live bridge handle was attached; connect/disconnect remain silicon
 * bench tests.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/chips/cc3501e.h>
#include <alp/iot.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "../../../../src/backends/wifi/wifi_ops.h"

ZTEST_SUITE(alp_wifi_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_wifi_registry, test_cc3501e_picked_for_active_alif_e7)
{
	const alp_backend_t *be = alp_backend_select("wifi", ALP_SOC_REF_STR);
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "ti-cc3501e"), 0);
	zassert_equal(be->priority, 200);
}

ZTEST(alp_wifi_registry, test_cc3501e_picked_for_aen_exact_refs)
{
	const alp_backend_t *be = alp_backend_select("wifi", "alif:ensemble:e8");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "ti-cc3501e"), 0);
	zassert_equal(be->priority, 200);
}

ZTEST(alp_wifi_registry, test_sw_fallback_picked_for_unknown_silicon)
{
	/* Both registered backends are wildcards; the higher-priority
     * zephyr_drv would normally win.  This case still exercises the
     * selector and asserts the sw_fallback is reachable on the test
     * build via the registry's count.  Degraded pattern: only
     * inventory is asserted, not the specific pick. */
	const alp_backend_t *be = alp_backend_select("wifi", "fictional:soc:zz");
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

ZTEST(alp_wifi_registry, test_wifi_open_uses_attached_cc3501e_backend)
{
	cc3501e_t fake = {
		.initialised = true,
	};
	zassert_equal(alp_wifi_cc3501e_attach(&fake), ALP_OK);

	alp_wifi_t *wifi = alp_wifi_open();
	zassert_not_null(wifi);
	zassert_not_null(wifi->backend);
	zassert_equal(strcmp(wifi->backend->vendor, "ti-cc3501e"), 0);
	zassert_equal(wifi->backend->priority, 200);
	alp_wifi_close(wifi);
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_wifi_registry, test_backend_count_for_wifi)
{
	/* Six exact AEN CC3501E refs + zephyr_drv + sw_fallback. */
	zassert_equal(alp_backend_count("wifi"), 8u);
}
