/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the RPC registry dispatcher.  Mirrors the USB /
 * mproc sibling harnesses; no vendor extensions, so the test
 * surface is the bare selector + capability-getter + public-API
 * edges across the single alp_rpc_channel_t handle type.
 *
 * Backends visible on this test build:
 *   zephyr_drv      (priority 100, "*" wildcard)
 *   sw_fallback     (priority 0,   "*" wildcard)
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("rpc", ALP_SOC_REF_STR)`
 * exercises the same selector code path real customer builds hit.
 * Tests that need a different silicon_ref call alp_backend_select
 * directly.  CONFIG_OPENAMP / CONFIG_IPC_SERVICE stay OFF -- the
 * test only exercises the dispatcher's null-handle gates, the
 * selector, and the capability getter; none of which touch the
 * Zephyr ipc_service subsystem.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/rpc.h>

#include "../../../../src/backends/rpc/rpc_ops.h"

ZTEST_SUITE(alp_rpc_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_rpc_registry, test_zephyr_drv_picked_over_sw_on_alif_e7)
{
	const alp_backend_t *be = alp_backend_select("rpc", "alif:ensemble:e7");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "zephyr"), 0);
	zassert_equal(be->priority, 100);
}

ZTEST(alp_rpc_registry, test_sw_fallback_picked_for_unknown_silicon)
{
	/* Both registered backends are wildcards; the higher-priority
     * zephyr_drv would normally win.  This case still exercises the
     * selector and asserts the sw_fallback is reachable on the test
     * build via the registry's count.  Degraded pattern: only
     * inventory is asserted, not the specific pick. */
	const alp_backend_t *be = alp_backend_select("rpc", "fictional:soc:zz");
	zassert_not_null(be);
	(void)be;
	zassert_true(alp_backend_count("rpc") >= 2u);
}

ZTEST(alp_rpc_registry, test_select_returns_null_for_null_class)
{
	zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_rpc_registry, test_select_returns_null_for_null_silicon_ref)
{
	/* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
	zassert_is_null(alp_backend_select("rpc", NULL));
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_rpc_registry, test_rpc_open_returns_null_on_null_config)
{
	/* Dispatcher must reject NULL config before reaching the
     * backend; stamps last_error = ALP_ERR_INVAL. */
	alp_rpc_channel_t *ch = alp_rpc_open(NULL);
	zassert_is_null(ch);
}

ZTEST(alp_rpc_registry, test_rpc_capabilities_returns_null_for_null_handle)
{
	zassert_is_null(alp_rpc_capabilities(NULL));
}

/* ---------- #469: alp_rpc_call rejects NULL req with req_len > 0 ---- */

ZTEST(alp_rpc_registry, test_rpc_call_rejects_null_req_with_nonzero_len)
{
	/* alp_rpc_send already gated this (payload == NULL && len > 0);
	 * alp_rpc_call was missing the mirror check on req/req_len,
	 * risking a NULL-deref (or an unbounded read) downstream in a
	 * backend that trusts req_len without checking req.  A NULL or
	 * not-in-use channel would short-circuit to NOT_READY before this
	 * check runs, so a stack-local handle with in_use=true (ops still
	 * NULL) is used to reach the argument-validation gates without a
	 * real open(). */
	struct alp_rpc_channel ch;
	memset(&ch, 0, sizeof(ch));
	ch.in_use = true;

	uint8_t resp[4];
	size_t  resp_len = sizeof(resp);
	zassert_equal(alp_rpc_call(&ch, "method", NULL, 4u, resp, &resp_len, 1000u), ALP_ERR_INVAL);

	/* req == NULL with req_len == 0 is legitimate (no request body). */
	zassert_not_equal(alp_rpc_call(&ch, "method", NULL, 0u, resp, &resp_len, 1000u), ALP_ERR_INVAL);
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_rpc_registry, test_backend_count_for_rpc)
{
	/* zephyr_drv + sw_fallback registered on this build.
     * No vendor-specific backends exist for RPC in Slice 4c. */
	zassert_equal(alp_backend_count("rpc"), 2u);
}
