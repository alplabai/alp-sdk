/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test doubles for the two seams src/backends/pwm/gd32_bridge.c reaches
 * through: the V2N supervisor singleton (normally src/zephyr/v2n_supervisor.c)
 * and the GD32G553 host driver's PWM entry points (normally
 * chips/gd32g553/gd32g553.c). Neither production file is linked into this
 * test target (see CMakeLists.txt), so every symbol gd32_bridge.c calls
 * needs a definition here -- acquire/release always succeed with a dummy
 * ctx; the gd32g553_pwm_* bodies are never exercised by this test (only
 * br_open() is under test) but must still resolve at link time because
 * gd32_bridge.c calls them unconditionally from its other ops.
 */

#include "alp/chips/gd32g553.h"
#include "v2n_supervisor.h"

static gd32g553_t s_fake_ctx;

alp_status_t alp_z_v2n_supervisor_acquire(gd32g553_t **ctx_out)
{
	*ctx_out = &s_fake_ctx;
	return ALP_OK;
}

void alp_z_v2n_supervisor_release(void)
{
}

alp_status_t
gd32g553_pwm_set(gd32g553_t *ctx, uint8_t channel, uint32_t period_ns, uint32_t duty_ns)
{
	(void)ctx;
	(void)channel;
	(void)period_ns;
	(void)duty_ns;
	return ALP_OK;
}

alp_status_t gd32g553_pwm_configure(gd32g553_t          *ctx,
                                    uint8_t              channel,
                                    gd32g553_pwm_align_t align_mode,
                                    uint32_t             dead_time_ns,
                                    uint8_t              break_cfg)
{
	(void)ctx;
	(void)channel;
	(void)align_mode;
	(void)dead_time_ns;
	(void)break_cfg;
	return ALP_OK;
}

alp_status_t gd32g553_pwm_single_pulse(gd32g553_t *ctx, uint8_t channel, uint32_t pulse_ns)
{
	(void)ctx;
	(void)channel;
	(void)pulse_ns;
	return ALP_OK;
}

alp_status_t gd32g553_pwm_capture_read(gd32g553_t *ctx,
                                       uint8_t     channel,
                                       uint32_t   *period_ns_out,
                                       uint32_t   *pulse_ns_out)
{
	(void)ctx;
	(void)channel;
	(void)period_ns_out;
	(void)pulse_ns_out;
	return ALP_ERR_NOSUPPORT;
}

alp_status_t gd32g553_pwm_capture_end(gd32g553_t *ctx, uint8_t channel)
{
	(void)ctx;
	(void)channel;
	return ALP_OK;
}
