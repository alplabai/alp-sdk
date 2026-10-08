/* SPDX-License-Identifier: Apache-2.0
 *
 * Dispatcher contract tests for the #2784 power surface: the per-mode
 * wake check and the SoM power-domain / boot-wake-info plumbing.
 *
 * A test-only "power" backend (priority 50, wildcard) out-ranks the
 * built-in stub.  It arms RTC | GPIO | TIMER overall but only RTC in
 * STOP, and implements none of the domain ops -- exactly the shape of
 * every shipping backend today, so the NOSUPPORT paths are the real
 * ones.  A second pass swaps in domain ops to prove the plumbing.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/peripheral.h>
#include <alp/power.h>

#include "../../../../src/backends/power/power_ops.h"

#define ALL_WAKE (ALP_POWER_WAKE_RTC | ALP_POWER_WAKE_GPIO | ALP_POWER_WAKE_TIMER)

static unsigned int g_sleep_calls;
static unsigned int g_policy_calls;

static alp_status_t
t_open(alp_power_backend_state_t *state, alp_capabilities_t *caps_out, uint32_t *wake_caps_out)
{
	(void)state;
	(void)caps_out;
	*wake_caps_out = ALL_WAKE;
	return ALP_OK;
}

static alp_status_t t_configure_wake_source(alp_power_backend_state_t *state, uint32_t bitmap)
{
	(void)state;
	(void)bitmap;
	return ALP_OK;
}

static alp_status_t t_request_sleep(alp_power_backend_state_t *state,
                                    alp_power_mode_t           mode,
                                    uint32_t                   wake_after_ms,
                                    alp_power_wake_info_t     *info)
{
	(void)state;
	(void)wake_after_ms;
	g_sleep_calls++;
	if (info != NULL) {
		info->realised_mode = mode;
	}
	return ALP_OK;
}

static uint32_t t_mode_wake_caps(const alp_power_backend_state_t *state, alp_power_mode_t mode)
{
	(void)state;
	if (mode == ALP_POWER_MODE_STANDBY) {
		return 0u; /* arms nothing: not even a timed wake */
	}
	return (mode == ALP_POWER_MODE_STOP) ? ALP_POWER_WAKE_RTC : ALL_WAKE;
}

static alp_status_t t_domain_policy_set(alp_power_backend_state_t *state,
                                        alp_power_domain_t         domain,
                                        alp_power_domain_policy_t  policy)
{
	(void)state;
	(void)domain;
	(void)policy;
	g_policy_calls++;
	return ALP_OK;
}

static alp_status_t t_domain_info(alp_power_domain_t domain, alp_power_domain_info_t *out)
{
	out->present           = (domain == ALP_POWER_DOMAIN_WIFI_BLE);
	out->default_action    = ALP_POWER_ACTION_HOLD_RESET;
	out->supported_actions = ALP_POWER_ACTION_HOLD_RESET;
	return ALP_OK;
}

static alp_status_t t_boot_wake_info(alp_power_boot_info_t *out)
{
	out->valid       = true;
	out->wake_source = ALP_POWER_WAKE_RTC;
	return ALP_OK;
}

/* Base ops: mode_wake_caps only; domain ops absent. */
static alp_power_ops_t _ops = {
	.open                  = t_open,
	.configure_wake_source = t_configure_wake_source,
	.request_sleep         = t_request_sleep,
	.mode_wake_caps        = t_mode_wake_caps,
};

ALP_BACKEND_REGISTER(power,
                     test_contract,
                     {
                         .silicon_ref = "*",
                         .vendor      = "test",
                         .base_caps   = 0u,
                         .priority    = 50,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

static void *suite_setup(void)
{
	return NULL;
}

static void before(void *fixture)
{
	(void)fixture;
	g_sleep_calls          = 0u;
	g_policy_calls         = 0u;
	_ops.mode_wake_caps    = t_mode_wake_caps;
	_ops.domain_policy_set = NULL;
	_ops.domain_info       = NULL;
	_ops.boot_wake_info    = NULL;
}

ZTEST_SUITE(alp_power_contract, NULL, suite_setup, before, NULL, NULL);

ZTEST(alp_power_contract, test_test_backend_wins_selection)
{
	const alp_backend_t *be = alp_backend_select("power", "alif:ensemble:e7");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "test"), 0);
}

ZTEST(alp_power_contract, test_mode_check_rejects_source_stop_cannot_arm)
{
	alp_power_t *h = alp_power_open();
	zassert_not_null(h);
	zassert_equal(alp_power_configure_wake_source(h, ALP_POWER_WAKE_GPIO), ALP_OK);

	/* GPIO is armable in SLEEP ... */
	zassert_equal(alp_power_request_sleep(h, ALP_POWER_MODE_SLEEP, 10u, NULL), ALP_OK);
	zassert_equal(g_sleep_calls, 1u);
	/* ... but not in STOP: refused before the backend runs. */
	zassert_equal(alp_power_request_sleep(h, ALP_POWER_MODE_STOP, 10u, NULL), ALP_ERR_NOSUPPORT);
	zassert_equal(g_sleep_calls, 1u, "backend must not run on a mode-check reject");
	alp_power_close(h);
}

ZTEST(alp_power_contract, test_mode_check_accepts_source_stop_can_arm)
{
	alp_power_t *h = alp_power_open();
	zassert_not_null(h);
	zassert_equal(alp_power_configure_wake_source(h, ALP_POWER_WAKE_RTC), ALP_OK);
	zassert_equal(alp_power_request_sleep(h, ALP_POWER_MODE_STOP, 10u, NULL), ALP_OK);
	zassert_equal(g_sleep_calls, 1u);
	alp_power_close(h);
}

ZTEST(alp_power_contract, test_mode_check_mixed_bitmap_rejected_whole)
{
	alp_power_t *h = alp_power_open();
	zassert_not_null(h);
	zassert_equal(alp_power_configure_wake_source(h, ALP_POWER_WAKE_RTC | ALP_POWER_WAKE_TIMER),
	              ALP_OK);
	zassert_equal(alp_power_request_sleep(h, ALP_POWER_MODE_STOP, 10u, NULL), ALP_ERR_NOSUPPORT);
	zassert_equal(g_sleep_calls, 0u);
	alp_power_close(h);
}

ZTEST(alp_power_contract, test_mode_check_skipped_for_timer_only_request)
{
	/* No configured source, wake_after_ms only: nothing to refuse. */
	alp_power_t *h = alp_power_open();
	zassert_not_null(h);
	zassert_equal(alp_power_request_sleep(h, ALP_POWER_MODE_STOP, 10u, NULL), ALP_OK);
	alp_power_close(h);
}

ZTEST(alp_power_contract, test_timed_wake_nosupport_when_mode_arms_no_timer)
{
	alp_power_t *h = alp_power_open();
	zassert_not_null(h);
	zassert_equal(alp_power_configure_wake_source(h, ALP_POWER_WAKE_NONE), ALP_OK);
	zassert_equal(alp_power_request_sleep(h, ALP_POWER_MODE_STANDBY, 10u, NULL), ALP_ERR_NOSUPPORT);
	zassert_equal(g_sleep_calls, 0u);
	/* wake_after_ms == 0 with a source the mode cannot arm is the other path. */
	zassert_equal(alp_power_configure_wake_source(h, ALP_POWER_WAKE_RTC), ALP_OK);
	zassert_equal(alp_power_request_sleep(h, ALP_POWER_MODE_STANDBY, 0u, NULL), ALP_ERR_NOSUPPORT);
	alp_power_close(h);
}

ZTEST(alp_power_contract, test_timed_wake_unchecked_without_mode_op)
{
	_ops.mode_wake_caps = NULL;
	alp_power_t *h      = alp_power_open();
	zassert_not_null(h);
	zassert_equal(alp_power_request_sleep(h, ALP_POWER_MODE_STANDBY, 10u, NULL), ALP_OK);
	alp_power_close(h);
}

ZTEST(alp_power_contract, test_domain_policy_set_closed_handle_not_ready)
{
	_ops.domain_policy_set = t_domain_policy_set;
	alp_power_t *h         = alp_power_open();
	zassert_not_null(h);
	alp_power_close(h);
	zassert_equal(
	    alp_power_domain_policy_set(h, ALP_POWER_DOMAIN_RTC, ALP_POWER_DOMAIN_POLICY_AUTO),
	    ALP_ERR_NOT_READY);
	zassert_equal(g_policy_calls, 0u);
}

ZTEST(alp_power_contract, test_domain_info_range_reject_zeroes_out)
{
	alp_power_domain_info_t info;
	memset(&info, 0xA5, sizeof(info));
	zassert_equal(alp_power_domain_info((alp_power_domain_t)-1, &info), ALP_ERR_INVAL);
	zassert_false(info.present);
	zassert_equal(info.supported_actions, 0u);
	zassert_equal(info.dependents, 0u);
	memset(&info, 0xA5, sizeof(info));
	zassert_equal(alp_power_domain_info(ALP_POWER_DOMAIN_COUNT, &info), ALP_ERR_INVAL);
	zassert_equal(info.default_action, 0u);
}

ZTEST(alp_power_contract, test_no_mode_op_falls_back_to_overall_caps)
{
	_ops.mode_wake_caps = NULL;
	alp_power_t *h      = alp_power_open();
	zassert_not_null(h);
	zassert_equal(alp_power_configure_wake_source(h, ALP_POWER_WAKE_GPIO), ALP_OK);
	zassert_equal(alp_power_request_sleep(h, ALP_POWER_MODE_STOP, 10u, NULL), ALP_OK);
	alp_power_close(h);
	_ops.mode_wake_caps = t_mode_wake_caps;
}

/* ---- Domain surface: NOSUPPORT when the backend lacks the ops ------ */

ZTEST(alp_power_contract, test_domain_policy_set_nosupport_without_op)
{
	alp_power_t *h = alp_power_open();
	zassert_not_null(h);
	zassert_equal(
	    alp_power_domain_policy_set(h, ALP_POWER_DOMAIN_WIFI_BLE, ALP_POWER_DOMAIN_POLICY_AUTO),
	    ALP_ERR_NOSUPPORT);
	alp_power_close(h);
}

ZTEST(alp_power_contract, test_domain_info_nosupport_and_zeroed)
{
	alp_power_domain_info_t info;
	memset(&info, 0xA5, sizeof(info));
	zassert_equal(alp_power_domain_info(ALP_POWER_DOMAIN_ETH_PHY, &info), ALP_ERR_NOSUPPORT);
	zassert_false(info.present);
	zassert_equal(info.supported_actions, 0u);
}

ZTEST(alp_power_contract, test_boot_wake_info_nosupport_and_zeroed)
{
	alp_power_boot_info_t bi;
	memset(&bi, 0xA5, sizeof(bi));
	zassert_equal(alp_power_boot_wake_info(&bi), ALP_ERR_NOSUPPORT);
	zassert_false(bi.valid);
	zassert_equal(bi.wake_source, 0u);
}

/* ---- Argument validation (backend-independent) --------------------- */

ZTEST(alp_power_contract, test_domain_argument_validation)
{
	alp_power_t *h = alp_power_open();
	zassert_not_null(h);
	_ops.domain_policy_set = t_domain_policy_set;
	zassert_equal(
	    alp_power_domain_policy_set(NULL, ALP_POWER_DOMAIN_RTC, ALP_POWER_DOMAIN_POLICY_AUTO),
	    ALP_ERR_NOT_READY);
	zassert_equal(
	    alp_power_domain_policy_set(h, ALP_POWER_DOMAIN_COUNT, ALP_POWER_DOMAIN_POLICY_AUTO),
	    ALP_ERR_INVAL);
	zassert_equal(
	    alp_power_domain_policy_set(h, (alp_power_domain_t)-1, ALP_POWER_DOMAIN_POLICY_AUTO),
	    ALP_ERR_INVAL);
	zassert_equal(
	    alp_power_domain_policy_set(h, ALP_POWER_DOMAIN_RTC, (alp_power_domain_policy_t)3),
	    ALP_ERR_INVAL);
	zassert_equal(g_policy_calls, 0u, "invalid args must not reach the backend");
	alp_power_close(h);

	alp_power_domain_info_t info;
	zassert_equal(alp_power_domain_info(ALP_POWER_DOMAIN_RTC, NULL), ALP_ERR_INVAL);
	zassert_equal(alp_power_domain_info(ALP_POWER_DOMAIN_COUNT, &info), ALP_ERR_INVAL);
	zassert_equal(alp_power_boot_wake_info(NULL), ALP_ERR_INVAL);
}

/* ---- Domain surface: plumbing reaches the backend ------------------ */

ZTEST(alp_power_contract, test_domain_ops_plumbed_to_backend)
{
	_ops.domain_policy_set = t_domain_policy_set;
	_ops.domain_info       = t_domain_info;
	_ops.boot_wake_info    = t_boot_wake_info;

	alp_power_t *h = alp_power_open();
	zassert_not_null(h);
	zassert_equal(alp_power_domain_policy_set(
	                  h, ALP_POWER_DOMAIN_WIFI_BLE, ALP_POWER_DOMAIN_POLICY_KEEP_ALIVE),
	              ALP_OK);
	zassert_equal(g_policy_calls, 1u);
	alp_power_close(h);

	alp_power_domain_info_t info;
	zassert_equal(alp_power_domain_info(ALP_POWER_DOMAIN_WIFI_BLE, &info), ALP_OK);
	zassert_true(info.present);
	zassert_equal(info.default_action, ALP_POWER_ACTION_HOLD_RESET);
	zassert_equal(alp_power_domain_info(ALP_POWER_DOMAIN_EXT_RAM, &info), ALP_OK);
	zassert_false(info.present);

	alp_power_boot_info_t bi;
	zassert_equal(alp_power_boot_wake_info(&bi), ALP_OK);
	zassert_true(bi.valid);
	zassert_equal(bi.wake_source, ALP_POWER_WAKE_RTC);
}

ZTEST(alp_power_contract, test_domain_bit_macro)
{
	zassert_equal(ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_WIFI_BLE), 1u);
	zassert_equal(ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_BACKLIGHT), 1u << 6);
	zassert_true(ALP_POWER_DOMAIN_BIT(ALP_POWER_DOMAIN_COUNT - 1) <= 0x7Fu);
}
