/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test doubles for the two seams src/backends/pwm/gd32_bridge.c reaches
 * through: the V2N supervisor singleton (normally src/zephyr/v2n_supervisor.c)
 * and the GD32G553 host driver's PWM entry points (normally
 * chips/gd32g553/gd32g553.c). Neither production file is linked into this
 * test target (see CMakeLists.txt).
 *
 * gd32g553_pwm_set() simulates the CORRECTED gd32-bridge firmware
 * (firmware/gd32-bridge/hal/gd32/pwm.c, issue #1730): a period beyond
 * GD32_STUB_PWM_MAX_PERIOD_NS -- the 16-bit counter's reach at the
 * bridge's fixed 216:1-prescaled 1 us tick, i.e. PWM_TIMER_ARR_MAX+1 ticks
 * -- is refused with ALP_ERR_NOSUPPORT (wire STATUS_NOSUPPORT via
 * status_from_wire) instead of silently clamped and reported ALP_OK. Every
 * ACCEPTED call records the period/duty it was asked to program, so the
 * test can assert what the "hardware" actually last saw.
 */

#include "alp/chips/gd32g553.h"
#include "v2n_supervisor.h"

static gd32g553_t s_fake_ctx;

/* 65536 ticks * 1000 ns/tick = 65,536,000 ns = 65.536 ms -- matches
 * PWM_TIMER_ARR_MAX+1 (0xFFFFu + 1) at the bridge's fixed 1 us tick
 * (firmware/gd32-bridge/hal/gd32/gd32_common.h: 216 MHz / 216 = 1 MHz). */
#define GD32_STUB_PWM_MAX_PERIOD_NS 65536000u

uint32_t g_stub_last_programmed_period_ns;
uint32_t g_stub_last_programmed_duty_ns;
uint32_t g_stub_pwm_set_calls;

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
	g_stub_pwm_set_calls++;
	if (period_ns > GD32_STUB_PWM_MAX_PERIOD_NS) {
		/* Refuse -- do NOT record this as the last-programmed value:
		 * the point under test is that the unprogrammed request never
		 * reaches the "hardware". */
		return ALP_ERR_NOSUPPORT;
	}
	g_stub_last_programmed_period_ns = period_ns;
	g_stub_last_programmed_duty_ns   = duty_ns;
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
