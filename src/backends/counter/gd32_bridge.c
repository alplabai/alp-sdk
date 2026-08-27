/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * V2N counter backend routed through the GD32G553 supervisor MCU.
 */

#include <stdbool.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/counter.h>
#include <alp/peripheral.h>

#include "counter_ops.h"
#include "v2n_supervisor.h"

static alp_status_t br_open(const alp_counter_config_t  *cfg,
                            alp_counter_backend_state_t *st,
                            alp_capabilities_t          *caps_out)
{
	/* include/alp/e1m_x_pinout.h publishes COUNTER0..3 as E1M-X
	 * connector identities -- a form-factor bound, not a per-SoM
	 * promise (alp-sdk#1242) -- and the V2N/V2M bridge serves only
	 * id 0.  INVAL (not NOSUPPORT) matches the adc / dac / pwm
	 * gd32-bridge siblings: "you asked for an instance that does not
	 * exist" is one question with one answer across this SoM vendor's
	 * SDK, and NOSUPPORT is reserved for "this build cannot do that at
	 * all" -- a different question (alp-sdk#1635).  caps_out->channel_count
	 * below (same field src/backends/adc/gd32_bridge.c populates)
	 * publishes the served count via alp_counter_capabilities() so a
	 * customer can discover it after opening COUNTER0, before hitting
	 * this on COUNTER1..3. */
	if (cfg->counter_id >= GD32G553_BRIDGE_COUNTER_CHANNELS) return ALP_ERR_INVAL;
	gd32g553_t  *ctx = NULL;
	alp_status_t s   = alp_z_v2n_supervisor_acquire(&ctx);
	if (s != ALP_OK) return s;
	alp_z_v2n_supervisor_release();
	st->dev                 = NULL; /* bridge sentinel */
	st->counter_id          = cfg->counter_id;
	st->be_data             = NULL;
	caps_out->flags         = 0u;                               /* no HW_ALARM via bridge */
	caps_out->channel_count = GD32G553_BRIDGE_COUNTER_CHANNELS; /* alp-sdk#1242 */
	return ALP_OK;
}

static alp_status_t br_start(alp_counter_backend_state_t *st)
{
	(void)st;
	return ALP_OK; /* counter free-runs on GD32 */
}

static alp_status_t br_stop(alp_counter_backend_state_t *st)
{
	(void)st;
	return ALP_OK; /* no stop opcode on bridge */
}

static alp_status_t br_get_value(alp_counter_backend_state_t *st, uint32_t *ticks_out)
{
	gd32g553_t  *ctx = NULL;
	alp_status_t s   = alp_z_v2n_supervisor_acquire(&ctx);
	if (s != ALP_OK) return s;
	s = gd32g553_counter_read(ctx, (uint8_t)st->counter_id, ticks_out);
	alp_z_v2n_supervisor_release();
	return s;
}

static alp_status_t
br_us_to_ticks(alp_counter_backend_state_t *st, uint32_t us, uint32_t *ticks_out)
{
	(void)st;
	(void)us;
	*ticks_out = 0u;
	return ALP_ERR_NOSUPPORT; /* v0.3 adds CMD_COUNTER_GET_FREQ */
}

static alp_status_t
br_set_alarm(alp_counter_backend_state_t *st, uint32_t ticks_from_now, struct alp_counter *owner)
{
	(void)st;
	(void)ticks_from_now;
	(void)owner;
	return ALP_ERR_NOSUPPORT; /* no IRQ line GD32 -> Renesas */
}

static alp_status_t br_cancel_alarm(alp_counter_backend_state_t *st)
{
	(void)st;
	return ALP_OK; /* no alarms ever armed */
}

static const alp_counter_ops_t _ops = {
	.open         = br_open,
	.start        = br_start,
	.stop         = br_stop,
	.get_value    = br_get_value,
	.us_to_ticks  = br_us_to_ticks,
	.set_alarm    = br_set_alarm,
	.cancel_alarm = br_cancel_alarm,
	.close        = NULL,
};

ALP_BACKEND_REGISTER(counter,
                     gd32_bridge,
                     {
                         .silicon_ref = "renesas:rzv2n:n44",
                         .vendor      = "renesas",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
