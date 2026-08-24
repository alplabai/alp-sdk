/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #1624 regression: src/backends/pwm/gd32_bridge.c's br_open() never
 * wrote the dispatcher-owned struct alp_pwm::period_ns field. pwm_dispatch.c
 * bounds-checks every alp_pwm_set_duty(pulse_ns) call against that field
 * (src/pwm_dispatch.c: `if (pulse_ns > pwm->period_ns) rc = ALP_ERR_INVAL;`),
 * so with the field left at its zeroed open-time value, every non-zero
 * pulse_ns fails -- the whole PWM duty surface was unusable on the V2N /
 * V2M product lines.
 *
 * Why this is a standalone unit test rather than an addition to
 * tests/zephyr/peripheral/src/pwm.c's alp_sdk.peripheral.v2n_supervisor
 * scenario: that scenario's prj_v2n_supervisor.conf deliberately leaves the
 * supervisor's SPI/I2C bus IDs unset, so alp_z_v2n_supervisor_acquire()
 * (src/zephyr/v2n_supervisor.c) always returns ALP_ERR_NOT_READY before
 * br_open() ever reaches the buggy line -- a ztest added there would only
 * ever see NOT_READY and pass vacuously regardless of whether the bug is
 * present. Reaching ALP_OK through the real supervisor singleton needs an
 * actual GD32 handshake over a real bus, which native_sim cannot provide
 * either. This test instead links the real br_open() directly (via the
 * backend registry, since br_open() is `static`) with the supervisor
 * acquire/release + gd32g553_pwm_* HAL seam stubbed to succeed
 * unconditionally (src/stubs.c) -- see this suite's CMakeLists.txt for why
 * the alp-sdk Zephyr module itself is not linked here.
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

ZTEST_SUITE(pwm_gd32_bridge_period, NULL, NULL, NULL, NULL, NULL);

ZTEST(pwm_gd32_bridge_period, test_br_open_writes_dispatcher_period_ns)
{
	const alp_backend_t *be = alp_backend_select("pwm", "renesas:rzv2n:n44");
	zassert_not_null(be, "gd32_bridge did not register for the V2N silicon_ref");

	const alp_pwm_ops_t *ops = (const alp_pwm_ops_t *)be->ops;
	zassert_not_null(ops);
	zassert_not_null(ops->open);

	const alp_pwm_config_t cfg = {
		.channel_id = 0u,
		.period_ns  = 1000000u, /* 1 kHz, matches the issue's repro */
		.polarity   = ALP_PWM_POLARITY_NORMAL,
	};
	struct alp_pwm     h    = { 0 };
	alp_capabilities_t caps = { 0 };

	const alp_status_t rc = ops->open(&cfg, &h.state, &caps);
	zassert_equal(rc, ALP_OK, "br_open() should succeed with the acquire stub");

	/* The bug: br_open() only ever wrote its private gd32_pwm_state_t's
	 * period_ns (be->state.be_data), never the dispatcher-owned handle
	 * field pwm_dispatch.c reads. Pre-fix this is 0 (the memset value
	 * from _alloc()); post-fix it matches the requested period. */
	zassert_equal(h.period_ns,
	              cfg.period_ns,
	              "struct alp_pwm::period_ns not set by br_open() -- got %u, want %u",
	              h.period_ns,
	              cfg.period_ns);

	/* Mirror pwm_dispatch.c's exact alp_pwm_set_duty() bounds check
	 * (src/pwm_dispatch.c:121) to tie this directly to the reported
	 * symptom: a 50% duty request must not be rejected as out of range
	 * against the handle's own configured period. */
	const uint32_t pulse_ns = cfg.period_ns / 2u;
	zassert_false(pulse_ns > h.period_ns,
	              "alp_pwm_set_duty(%u) would incorrectly return ALP_ERR_INVAL",
	              pulse_ns);
}
