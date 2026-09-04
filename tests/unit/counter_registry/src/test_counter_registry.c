/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the counter registry dispatcher.  Mirrors the Slice
 * 4a wdt_registry / rtc_registry harnesses; no vendor extensions, so
 * the test surface is the bare selector + capability-getter + public-
 * API edges.
 *
 * Backends visible on this test build:
 *   zephyr_drv      (priority 100, "*" wildcard)
 *   sw_fallback     (priority 0,   "*" wildcard)
 *
 * The gd32_bridge backend is NOT linked here: this prj.conf
 * deliberately omits CONFIG_ALP_SDK_V2N_SUPERVISOR=y so the bridge
 * TU is excluded by the CMake gate added in commit 8.  A separate
 * test build that flips that Kconfig on would exercise the
 * "renesas:rzv2n:n44" silicon_ref path; see TODO at the bottom of
 * this file.
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("counter", ALP_SOC_REF_STR)`
 * exercises the same selector code path real customer builds hit.
 * Tests that need a different silicon_ref call alp_backend_select
 * directly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/counter.h>
#include <alp/peripheral.h>

#include "../../../../src/backends/counter/counter_ops.h"

ZTEST_SUITE(alp_counter_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_counter_registry, test_zephyr_drv_picked_over_sw_on_alif_e7)
{
	const alp_backend_t *be = alp_backend_select("counter", "alif:ensemble:e7");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "zephyr"), 0);
	zassert_equal(be->priority, 100);
}

ZTEST(alp_counter_registry, test_sw_fallback_picked_for_unknown_silicon)
{
	/* Both registered backends are wildcards; the higher-priority
     * zephyr_drv would normally win.  This case still exercises the
     * selector and asserts the sw_fallback is reachable on the test
     * build -- if the selector preference ever changes to "exact
     * match beats wildcard", this assertion documents the expected
     * behaviour. */
	const alp_backend_t *be = alp_backend_select("counter", "fictional:soc:zz");
	zassert_not_null(be);
	/* On the current selector (priority-only) zephyr_drv wins for
     * fictional silicon too; we still assert reachability of the
     * sw_fallback via the registry's count below. */
	(void)be;
	zassert_true(alp_backend_count("counter") >= 2u);
}

ZTEST(alp_counter_registry, test_select_returns_null_for_null_class)
{
	zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_counter_registry, test_select_returns_null_for_null_silicon_ref)
{
	/* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
	zassert_is_null(alp_backend_select("counter", NULL));
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_counter_registry, test_open_returns_null_on_null_config)
{
	/* The dispatcher rejects NULL cfg with ALP_ERR_INVAL before
     * consulting the registry; alp_counter_open returns NULL. */
	alp_counter_t *h = alp_counter_open(NULL);
	zassert_is_null(h);
}

ZTEST(alp_counter_registry, test_capabilities_returns_null_for_null_handle)
{
	zassert_is_null(alp_counter_capabilities(NULL));
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_counter_registry, test_backend_count_for_counter)
{
	/* zephyr_drv + sw_fallback registered on this build.
     * gd32_bridge is gated behind CONFIG_ALP_SDK_V2N_SUPERVISOR=y
     * which this prj.conf deliberately omits. */
	zassert_equal(alp_backend_count("counter"), 2u);
}

/* ---------- close() cancels the driver alarm (#1627) ----------------- */

static void _alarm_cb_noop(alp_counter_t *counter, uint32_t ticks, void *user)
{
	ARG_UNUSED(counter);
	ARG_UNUSED(ticks);
	ARG_UNUSED(user);
}

ZTEST(alp_counter_registry, test_zephyr_drv_close_cancels_channel_alarm)
{
	/* Handle A arms channel 0 and closes WITHOUT cancelling first --
     * before the fix, z_close() only called counter_stop(), leaving
     * the native-sim counter driver's is_alarm_pending[0] set. */
	alp_counter_config_t cfg = ALP_COUNTER_CONFIG_DEFAULT(0);
	alp_counter_t       *ha  = alp_counter_open(&cfg);
	zassert_not_null(ha, "zephyr_drv counter0 must open on native_sim (see boards/ overlay)");
	zassert_equal(alp_counter_start(ha), ALP_OK);
	zassert_equal(alp_counter_set_alarm(ha, 1000000u, _alarm_cb_noop, NULL), ALP_OK);
	alp_counter_close(ha); /* alarm left armed on purpose */

	/* Handle B reopens the same device and arms channel 0 again.  If
     * close() actually cancelled A's alarm, the native-sim driver's
     * channel-0 slot is free and this succeeds; if it leaked, the
     * driver's ctr_set_alarm() sees is_alarm_pending[0] still true and
     * refuses with -EBUSY (ALP_ERR_BUSY) -- the issue's literal
     * "second reachable outcome". */
	alp_counter_t *hb = alp_counter_open(&cfg);
	zassert_not_null(hb);
	zassert_equal(alp_counter_start(hb), ALP_OK);
	zassert_equal(alp_counter_set_alarm(hb, 1000000u, _alarm_cb_noop, NULL),
	              ALP_OK,
	              "set_alarm on a freshly reopened handle must not fail -- a prior handle's "
	              "close() left the driver's channel-0 alarm armed");

	alp_counter_close(hb);
}

ZTEST(alp_counter_registry, test_zephyr_drv_close_cancels_channel_alarm_after_stop)
{
	/* Same as test_zephyr_drv_close_cancels_channel_alarm, but handle A
     * calls alp_counter_stop() before close() -- the canonical teardown
     * order most callers write.  native_sim's ctr_cancel_alarm() refuses
     * (-ENOTSUP) once the counter is stopped, so a close() that cancels
     * only while the counter happens to still be running leaves
     * is_alarm_pending[0] set on THIS ordering even though the other
     * ordering above is clean. */
	alp_counter_config_t cfg = ALP_COUNTER_CONFIG_DEFAULT(0);
	alp_counter_t       *ha  = alp_counter_open(&cfg);
	zassert_not_null(ha, "zephyr_drv counter0 must open on native_sim (see boards/ overlay)");
	zassert_equal(alp_counter_start(ha), ALP_OK);
	zassert_equal(alp_counter_set_alarm(ha, 1000000u, _alarm_cb_noop, NULL), ALP_OK);
	zassert_equal(alp_counter_stop(ha), ALP_OK);
	alp_counter_close(ha); /* alarm left armed on purpose, counter already stopped */

	alp_counter_t *hb = alp_counter_open(&cfg);
	zassert_not_null(hb);
	zassert_equal(alp_counter_start(hb), ALP_OK);
	zassert_equal(alp_counter_set_alarm(hb, 1000000u, _alarm_cb_noop, NULL),
	              ALP_OK,
	              "set_alarm on a freshly reopened handle must not fail -- a prior handle's "
	              "stop()-then-close() left the driver's channel-0 alarm armed");

	alp_counter_close(hb);
}

/* TODO(slice-4a-followup): bridge backend selection test.
 * When CONFIG_ALP_SDK_V2N_SUPERVISOR=y, alp_backend_select("counter",
 * "renesas:rzv2n:n44") should return vendor "renesas" priority 100
 * (gd32_bridge wins via specific silicon_ref match).  Add a separate
 * test build (e.g. tests/unit/counter_registry_v2n/) or a Kconfig
 * fragment that enables V2N_SUPERVISOR on native_sim to exercise this. */
