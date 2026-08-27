/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * V2N PWM backend routed through the GD32G553 supervisor MCU.
 *
 * RZ/V2N drives zero native E1M PWMs -- all eight map to GD32 TIMER0 /
 * TIMER7 channels (see gd32g553_pwm_configure docs).  This backend
 * mirrors the adc / counter / qenc bridges: every op acquires the
 * shared supervisor mutex and forwards to the gd32g553_pwm_* host
 * driver.  With no bus configured the supervisor-acquire returns
 * ALP_ERR_NOT_READY, which the dispatcher relays from open() -- the
 * same contract the adc / dac / counter bridges present.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/pwm.h>

#include "pwm_ops.h"
#include "alp_slot_claim.h"
#include "v2n_supervisor.h"
#include <alp/chips/gd32g553.h>

typedef struct gd32_pwm_state {
	uint8_t  channel_id;
	uint32_t period_ns;
	uint32_t duty_ns;
	bool     in_use;
} gd32_pwm_state_t;

#ifndef CONFIG_ALP_SDK_MAX_PWM_HANDLES
#define CONFIG_ALP_SDK_MAX_PWM_HANDLES 8
#endif

static gd32_pwm_state_t _state_pool[CONFIG_ALP_SDK_MAX_PWM_HANDLES];

/* issue #1115 round-2 dev review: claim atomically (in_use is the LAST
 * member; memset only the bytes ahead of it) instead of the previous
 * plain check-then-set scan. */
static gd32_pwm_state_t *_alloc_state(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_PWM_HANDLES; ++i) {
		if (alp_slot_try_claim(&_state_pool[i].in_use)) {
			memset(&_state_pool[i], 0, offsetof(gd32_pwm_state_t, in_use));
			return &_state_pool[i];
		}
	}
	return NULL;
}

static void _free_state(gd32_pwm_state_t *s)
{
	alp_slot_release(&s->in_use);
}

static alp_status_t
br_open(const alp_pwm_config_t *cfg, alp_pwm_backend_state_t *st, alp_capabilities_t *caps_out)
{
	/* E1M spec reserves 8 PWM channels; ALL EIGHT map to a real GD32
     * timer/channel on V2N (metadata/chips/gd32g553.yaml pwm_routing:
     * PWM0..7 -> TIMER0/TIMER7, no gaps) -- unlike zephyr_drv.c /
     * yocto_drv.c, this backend has no partial-population case to tell
     * apart from a malformed index, so a single ALP_ERR_INVAL is the
     * complete (not collapsed) answer here (#1635).  The two-tier
     * contract still holds for THIS backend overall: the real hardware
     * ceiling -- a period/duty the GD32 timer can't represent (16-bit
     * ARR at 216 MHz CK_TIMER, ~303 us longest period, see
     * docs/gd32-bridge-protocol.md §3.8) -- surfaces as
     * ALP_ERR_OUT_OF_RANGE from br_set_duty()/br_set_period() via the
     * firmware's own STATUS_OUT_OF_RANGE (wire 0x08, see
     * gd32g553.c:status_from_wire()), not from this channel_id check. */
	if (cfg->channel_id >= 8u) {
		return ALP_ERR_INVAL;
	}
	/* Probe the supervisor up-front: with no SPI/I2C bus configured
     * this returns ALP_ERR_NOT_READY, which the dispatcher relays. */
	gd32g553_t  *ctx = NULL;
	alp_status_t s   = alp_z_v2n_supervisor_acquire(&ctx);
	if (s != ALP_OK) {
		return s;
	}
	alp_z_v2n_supervisor_release();

	gd32_pwm_state_t *bs = _alloc_state();
	if (bs == NULL) {
		return ALP_ERR_NOMEM;
	}
	bs->channel_id = (uint8_t)cfg->channel_id;
	bs->period_ns  = cfg->period_ns;
	bs->duty_ns    = 0u;

	st->dev         = NULL; /* bridge sentinel */
	st->channel_id  = cfg->channel_id;
	st->be_data     = bs;
	caps_out->flags = 0u; /* no HW dead-time/break advertised via bridge */
	return ALP_OK;
}

static alp_status_t br_set_duty(alp_pwm_backend_state_t *st, uint32_t pulse_ns)
{
	gd32_pwm_state_t *bs = (gd32_pwm_state_t *)st->be_data;
	if (bs == NULL) return ALP_ERR_NOT_READY;
	gd32g553_t  *ctx = NULL;
	alp_status_t s   = alp_z_v2n_supervisor_acquire(&ctx);
	if (s != ALP_OK) return s;
	s = gd32g553_pwm_set(ctx, bs->channel_id, bs->period_ns, pulse_ns);
	alp_z_v2n_supervisor_release();
	if (s == ALP_OK) bs->duty_ns = pulse_ns;
	return s;
}

static alp_status_t br_set_period(alp_pwm_backend_state_t *st, uint32_t period_ns)
{
	gd32_pwm_state_t *bs = (gd32_pwm_state_t *)st->be_data;
	if (bs == NULL) return ALP_ERR_NOT_READY;
	gd32g553_t  *ctx = NULL;
	alp_status_t s   = alp_z_v2n_supervisor_acquire(&ctx);
	if (s != ALP_OK) return s;
	s = gd32g553_pwm_set(ctx, bs->channel_id, period_ns, bs->duty_ns);
	alp_z_v2n_supervisor_release();
	if (s == ALP_OK) bs->period_ns = period_ns;
	return s;
}

static alp_status_t br_configure(alp_pwm_backend_state_t *st,
                                 alp_pwm_align_t          align_mode,
                                 uint32_t                 dead_time_ns,
                                 uint8_t                  break_cfg)
{
	gd32_pwm_state_t *bs = (gd32_pwm_state_t *)st->be_data;
	if (bs == NULL) return ALP_ERR_NOT_READY;
	gd32g553_t  *ctx = NULL;
	alp_status_t s   = alp_z_v2n_supervisor_acquire(&ctx);
	if (s != ALP_OK) return s;
	/* alp_pwm_align_t and gd32g553_pwm_align_t share the same 0..3
     * EDGE / CENTER_{UP,DOWN,BOTH} ordering. */
	s = gd32g553_pwm_configure(
	    ctx, bs->channel_id, (gd32g553_pwm_align_t)align_mode, dead_time_ns, break_cfg);
	alp_z_v2n_supervisor_release();
	return s;
}

static alp_status_t br_single_pulse(alp_pwm_backend_state_t *st, uint32_t pulse_ns)
{
	gd32_pwm_state_t *bs = (gd32_pwm_state_t *)st->be_data;
	if (bs == NULL) return ALP_ERR_NOT_READY;
	gd32g553_t  *ctx = NULL;
	alp_status_t s   = alp_z_v2n_supervisor_acquire(&ctx);
	if (s != ALP_OK) return s;
	s = gd32g553_pwm_single_pulse(ctx, bs->channel_id, pulse_ns);
	alp_z_v2n_supervisor_release();
	return s;
}

static alp_status_t br_capture_open(const alp_pwm_capture_config_t *cfg,
                                    alp_pwm_backend_state_t        *st,
                                    alp_capabilities_t             *caps_out)
{
	/* PWM input-capture over the bridge is not wired in v0.7.  Report
     * unsupported (the general peripheral suite asserts NOSUPPORT for
     * capture on builds without a capture backend); the *_read/_close
     * ops below stay as paper-correct routes for when it lands but are
     * unreachable while open reports NOSUPPORT. */
	(void)cfg;
	(void)st;
	(void)caps_out;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t
br_capture_read(alp_pwm_backend_state_t *st, uint32_t *period_ns_out, uint32_t *pulse_ns_out)
{
	gd32_pwm_state_t *bs = (gd32_pwm_state_t *)st->be_data;
	if (bs == NULL) return ALP_ERR_NOT_READY;
	gd32g553_t  *ctx = NULL;
	alp_status_t s   = alp_z_v2n_supervisor_acquire(&ctx);
	if (s != ALP_OK) return s;
	s = gd32g553_pwm_capture_read(ctx, bs->channel_id, period_ns_out, pulse_ns_out);
	alp_z_v2n_supervisor_release();
	return s;
}

static void br_capture_close(alp_pwm_backend_state_t *st)
{
	gd32_pwm_state_t *bs = (gd32_pwm_state_t *)st->be_data;
	if (bs == NULL) return;
	gd32g553_t *ctx = NULL;
	if (alp_z_v2n_supervisor_acquire(&ctx) == ALP_OK) {
		(void)gd32g553_pwm_capture_end(ctx, bs->channel_id);
		alp_z_v2n_supervisor_release();
	}
	_free_state(bs);
	st->be_data = NULL;
}

static void br_close(alp_pwm_backend_state_t *st)
{
	if (st->be_data != NULL) {
		_free_state((gd32_pwm_state_t *)st->be_data);
		st->be_data = NULL;
	}
}

static const alp_pwm_ops_t _ops = {
	.open          = br_open,
	.set_duty      = br_set_duty,
	.set_period    = br_set_period,
	.configure     = br_configure,
	.single_pulse  = br_single_pulse,
	.capture_open  = br_capture_open,
	.capture_read  = br_capture_read,
	.capture_close = br_capture_close,
	.close         = br_close,
};

ALP_BACKEND_REGISTER(pwm,
                     gd32_bridge,
                     {
                         .silicon_ref = "renesas:rzv2n:n44",
                         .vendor      = "renesas", /* SoC vendor, not the bridge chip */
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
