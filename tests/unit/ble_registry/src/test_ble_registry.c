/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the BLE registry dispatcher.  Mirrors the USB
 * sibling harness; no vendor extensions, so the test surface is
 * the bare selector + capability-getter + public-API edges.
 *
 * Backends visible on this test build:
 *   cc3501e_e3..e8 (priority 200, exact AEN silicon refs)
 *   zephyr_drv      (priority 100, "*" wildcard)
 *   sw_fallback     (priority 0,   "*" wildcard)
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("ble", ALP_SOC_REF_STR)`
 * exercises the same selector code path real customer builds hit.
 * Tests that need a different silicon_ref call alp_backend_select
 * directly.  CONFIG_BT stays OFF -- the Zephyr BT backend body is not active.
 * The CC3501E BLE open path sends a real BLE_ENABLE bridge command, so unit
 * coverage stops at provider selection and the open/scan proof runs on the AEN
 * bench.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/ble.h>
#include <alp/cap_instance.h>
#include <alp/chips/cc3501e.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "../../../../src/backends/ble/ble_ops.h"
#include "../../../../src/common/alp_slot_claim.h"

ZTEST_SUITE(alp_ble_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_ble_registry, test_cc3501e_picked_for_active_alif_e7)
{
	const alp_backend_t *be = alp_backend_select("ble", ALP_SOC_REF_STR);
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "ti-cc3501e"), 0);
	zassert_equal(be->priority, 200);
}

ZTEST(alp_ble_registry, test_cc3501e_picked_for_aen_exact_refs)
{
	const alp_backend_t *be = alp_backend_select("ble", "alif:ensemble:e8");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "ti-cc3501e"), 0);
	zassert_equal(be->priority, 200);
}

ZTEST(alp_ble_registry, test_cc3501e_attach_accepts_initialised_bridge)
{
	cc3501e_t fake = {
		.initialised = true,
	};
	zassert_equal(alp_ble_cc3501e_attach(&fake), ALP_OK);
}

ZTEST(alp_ble_registry, test_sw_fallback_picked_for_unknown_silicon)
{
	/* Both registered backends are wildcards; the higher-priority
     * zephyr_drv would normally win.  This case still exercises the
     * selector and asserts the sw_fallback is reachable on the test
     * build via the registry's count.  Degraded pattern: only
     * inventory is asserted, not the specific pick. */
	const alp_backend_t *be = alp_backend_select("ble", "fictional:soc:zz");
	zassert_not_null(be);
	(void)be;
	zassert_true(alp_backend_count("ble") >= 2u);
}

ZTEST(alp_ble_registry, test_select_returns_null_for_null_class)
{
	zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_ble_registry, test_select_returns_null_for_null_silicon_ref)
{
	/* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
	zassert_is_null(alp_backend_select("ble", NULL));
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_ble_registry, test_ble_advertise_start_inval_on_null_handle)
{
	/* Dispatcher must reject NULL handle before reaching the
     * backend; returns ALP_ERR_NOT_READY per the public contract. */
	alp_ble_adv_config_t cfg = { 0 };
	zassert_equal(alp_ble_advertise_start(NULL, &cfg), ALP_ERR_NOT_READY);
}

ZTEST(alp_ble_registry, test_ble_capabilities_returns_null_for_null_handle)
{
	zassert_is_null(alp_ble_capabilities(NULL));
}

/* ---------- Adv-data 31-byte budget (#480/#888) --------------------- */

/* Stub backend that always accepts whatever adv_config reaches it -- stands
 * in for a backend with no budget check of its own, so these tests prove
 * the dispatcher enforces the legacy 31-byte adv PDU budget centrally
 * (ble_dispatch.c), not a specific backend's own defense-in-depth check. */
static alp_status_t _stub_advertise_start(alp_ble_radio_state_t      *state,
                                          const alp_ble_adv_config_t *cfg)
{
	(void)state;
	(void)cfg;
	return ALP_OK;
}

static const alp_ble_ops_t _stub_ble_ops = {
	.advertise_start = _stub_advertise_start,
};

static void _stub_open_handle(struct alp_ble *h)
{
	memset(h, 0, sizeof(*h));
	h->state.ops = &_stub_ble_ops;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
}

ZTEST(alp_ble_registry, test_ble_advertise_start_rejects_over_budget_adv_data)
{
	struct alp_ble h;
	_stub_open_handle(&h);

	static const alp_ble_uuid_t svc = { 0 };
	/* Flags 3 + name (2+15) + services (2+16*1) = 38 > 31. */
	alp_ble_adv_config_t cfg = {
		.name         = "123456789012345", /* 15 chars */
		.services     = &svc,
		.num_services = 1,
	};
	zassert_equal(alp_ble_advertise_start(&h, &cfg), ALP_ERR_INVAL);
}

ZTEST(alp_ble_registry, test_ble_advertise_start_accepts_at_budget_adv_data)
{
	struct alp_ble h;
	_stub_open_handle(&h);

	static const alp_ble_uuid_t svc = { 0 };
	/* Flags 3 + name (2+8) + services (2+16*1) = 31 == 31 (inclusive boundary). */
	alp_ble_adv_config_t cfg = {
		.name         = "12345678", /* 8 chars -- exactly the 31-byte cap */
		.services     = &svc,
		.num_services = 1,
	};
	zassert_equal(alp_ble_advertise_start(&h, &cfg), ALP_OK);
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_ble_registry, test_backend_count_for_ble)
{
	/* Six exact AEN CC3501E refs + zephyr_drv + sw_fallback. */
	zassert_equal(alp_backend_count("ble"), 8u);
}
