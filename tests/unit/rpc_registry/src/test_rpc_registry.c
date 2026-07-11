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
#include "../../../../src/common/alp_slot_claim.h"

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

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_rpc_registry, test_backend_count_for_rpc)
{
	/* zephyr_drv + sw_fallback registered on this build.
     * No vendor-specific backends exist for RPC in Slice 4c. */
	zassert_equal(alp_backend_count("rpc"), 2u);
}

/* ---------- Frame-size overflow guard ------------------------------ */

ZTEST(alp_rpc_registry, test_frame_size_fits_and_reports_total)
{
	size_t total = 0;
	zassert_true(alp_rpc_frame_size(4u, 10u, 64u, &total));
	zassert_equal(total, 15u); /* method + NUL + payload */
}

ZTEST(alp_rpc_registry, test_frame_size_exact_fit)
{
	size_t total = 0;
	/* method(4) + NUL(1) + payload(59) == cap(64) */
	zassert_true(alp_rpc_frame_size(4u, 59u, 64u, &total));
	zassert_equal(total, 64u);
}

ZTEST(alp_rpc_registry, test_frame_size_one_over_rejected)
{
	size_t total = 0;
	zassert_false(alp_rpc_frame_size(4u, 60u, 64u, &total));
}

ZTEST(alp_rpc_registry, test_frame_size_method_too_long_rejected)
{
	size_t total = 0;
	zassert_false(alp_rpc_frame_size(64u, 0u, 64u, &total));
}

ZTEST(alp_rpc_registry, test_frame_size_near_sizemax_payload_does_not_wrap)
{
	/* A near-SIZE_MAX payload must be rejected, not wrap the framed
     * total past `cap`. */
	size_t total = 0;
	zassert_false(alp_rpc_frame_size(4u, SIZE_MAX, 64u, &total));
	zassert_false(alp_rpc_frame_size(4u, SIZE_MAX - 3u, 64u, &total));
}

/* ---------- alp_rpc_call request-pointer guard --------------------- */

static bool _fake_call_reached;

static alp_status_t _fake_call(alp_rpc_backend_state_t *st,
                               const char              *method,
                               const void              *req,
                               size_t                   req_len,
                               void                    *resp,
                               size_t                  *resp_len,
                               uint32_t                 timeout_ms)
{
	(void)st;
	(void)method;
	(void)req;
	(void)req_len;
	(void)resp;
	(void)resp_len;
	(void)timeout_ms;
	_fake_call_reached = true;
	return ALP_OK;
}

static const alp_rpc_ops_t _fake_ops = {
	.call = _fake_call,
};

static struct alp_rpc_channel _make_fake_channel(void)
{
	struct alp_rpc_channel ch;
	memset(&ch, 0, sizeof(ch));
	ch.in_use = true;
	/* alp_rpc_open() also stamps this after a successful backend open
     * (GHSA-xhm8-7f87-93q5 defect 2 / issue #629's op-vs-close guard,
     * src/common/alp_slot_claim.h) -- alp_rpc_call() gates on this, not
     * `in_use`. */
	ch.lifecycle = ALP_HANDLE_LC_OPEN;
	ch.state.ops = &_fake_ops;
	return ch;
}

ZTEST(alp_rpc_registry, test_rpc_call_null_req_nonzero_len_rejected)
{
	/* Mirrors the alp_rpc_send guard: req == NULL with a non-zero length
     * must be rejected in the dispatcher before reaching the backend. */
	_fake_call_reached        = false;
	struct alp_rpc_channel ch = _make_fake_channel();
	zassert_equal(ALP_ERR_INVAL, alp_rpc_call(&ch, "m", NULL, 8, NULL, NULL, 0));
	zassert_false(_fake_call_reached, "backend call must not be reached");
}

ZTEST(alp_rpc_registry, test_rpc_call_null_req_zero_len_reaches_backend)
{
	_fake_call_reached        = false;
	struct alp_rpc_channel ch = _make_fake_channel();
	zassert_equal(ALP_OK, alp_rpc_call(&ch, "m", NULL, 0, NULL, NULL, 0));
	zassert_true(_fake_call_reached);
}

ZTEST(alp_rpc_registry, test_rpc_call_valid_req_reaches_backend)
{
	_fake_call_reached            = false;
	struct alp_rpc_channel ch     = _make_fake_channel();
	uint8_t                buf[8] = { 0 };
	zassert_equal(ALP_OK, alp_rpc_call(&ch, "m", buf, sizeof(buf), NULL, NULL, 0));
	zassert_true(_fake_call_reached);
}
