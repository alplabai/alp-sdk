/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #1730 regression: the gd32-bridge firmware used to silently clamp
 * an over-long PWM period to 65.536 ms (PWM_TIMER_ARR_MAX+1 ticks at the
 * bridge's fixed 1 us tick) and report BRIDGE_HW_OK -- so
 * src/backends/pwm/gd32_bridge.c's br_set_period() cached the *requested*
 * period, not what the pad actually ran at, and every later
 * alp_pwm_set_duty() re-sent the poisoned cached period on the wire.
 *
 * This test exercises the REAL br_set_period()/br_set_duty() (via the
 * backend registry, since both are `static`) against a stub HAL that
 * simulates the CORRECTED firmware: it refuses a period beyond what the
 * 16-bit counter can express with ALP_ERR_NOSUPPORT rather than silently
 * programming a clamped value (see src/stubs.c).
 *
 * Two things are pinned:
 *   1. The refusal propagates to the caller as ALP_ERR_NOSUPPORT, not
 *      ALP_OK and not ALP_ERR_INVAL (a malformed argument this is not --
 *      the period is a well-formed uint32_t the silicon simply can't
 *      represent; see ADR-0002's amended convention).
 *   2. The backend's private cache (gd32_pwm_state_t::period_ns, reached
 *      through bs->period_ns in gd32_bridge.c) is NOT poisoned by the
 *      refused request: a subsequent set_duty() re-sends the last
 *      SUCCESSFULLY programmed period on the wire, never the refused one.
 *      The stub's g_stub_last_programmed_period_ns is the observable
 *      proxy for "what the wire last carried".
 */

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/pwm.h>

#include "pwm_ops.h"

/* Normally instantiated once by src/pwm_dispatch.c; this test links
 * src/backends/pwm/gd32_bridge.c without the dispatcher, so it must supply
 * its own class-range entry for the selector (src/backend.c) to walk. */
ALP_BACKEND_DEFINE_CLASS(pwm);

extern uint32_t g_stub_last_programmed_period_ns;
extern uint32_t g_stub_last_programmed_duty_ns;
extern uint32_t g_stub_pwm_set_calls;

ZTEST_SUITE(pwm_gd32_bridge_period_refusal, NULL, NULL, NULL, NULL, NULL);

ZTEST(pwm_gd32_bridge_period_refusal, test_over_long_period_refused_not_clamped)
{
	const alp_backend_t *be = alp_backend_select("pwm", "renesas:rzv2n:n44");
	zassert_not_null(be, "gd32_bridge did not register for the V2N silicon_ref");

	const alp_pwm_ops_t *ops = (const alp_pwm_ops_t *)be->ops;
	zassert_not_null(ops);
	zassert_not_null(ops->open);
	zassert_not_null(ops->set_period);
	zassert_not_null(ops->set_duty);

	/* Open at a legitimate 20 us period (well within the 65.536 ms
	 * ceiling). */
	const alp_pwm_config_t cfg = {
		.channel_id = 0u,
		.period_ns  = 20000u,
		.polarity   = ALP_PWM_POLARITY_NORMAL,
	};
	struct alp_pwm     h    = { 0 };
	alp_capabilities_t caps = { 0 };
	alp_status_t        rc  = ops->open(&cfg, &h.state, &caps);
	zassert_equal(rc, ALP_OK, "br_open() should succeed with the acquire stub");
	zassert_equal(g_stub_last_programmed_period_ns,
	              0u,
	              "br_open() must not itself program the timer (no PWM_SET on open)");

	/* 100 ms -- beyond the 65.536 ms the bridge's 16-bit prescaled
	 * counter can express (issue #1730's repro value). */
	rc = ops->set_period(&h.state, 100000000u);
	zassert_equal(rc,
	              ALP_ERR_NOSUPPORT,
	              "an unrepresentable period must be refused ALP_ERR_NOSUPPORT, got %d",
	              rc);
	zassert_equal(g_stub_pwm_set_calls, 1u, "the refused request must still reach the HAL once");
	zassert_equal(g_stub_last_programmed_period_ns,
	              0u,
	              "the refused 100 ms request must never be programmed onto the wire");

	/* The cache must not be poisoned: a subsequent set_duty() re-sends
	 * the last SUCCESSFULLY programmed period (20 us from open), not the
	 * refused 100 ms request and not 0. */
	rc = ops->set_duty(&h.state, 5000u);
	zassert_equal(rc, ALP_OK, "set_duty at a valid pulse should succeed");
	zassert_equal(g_stub_last_programmed_period_ns,
	              cfg.period_ns,
	              "gd32_pwm_state_t::period_ns was poisoned by the refused set_period -- "
	              "wire got %u, want the original %u",
	              g_stub_last_programmed_period_ns,
	              cfg.period_ns);
	zassert_equal(g_stub_last_programmed_duty_ns, 5000u);
}
