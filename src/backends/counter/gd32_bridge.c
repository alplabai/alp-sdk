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

static alp_status_t br_open(const alp_counter_config_t *cfg, alp_counter_backend_state_t *st,
                            alp_capabilities_t *caps_out)
{
	if (cfg->counter_id >= 1u) return ALP_ERR_INVAL; /* bridge: only id 0 */
	gd32g553_t  *ctx = NULL;
	alp_status_t s   = alp_z_v2n_supervisor_acquire(&ctx);
	if (s != ALP_OK) return s;
	alp_z_v2n_supervisor_release();
	st->dev         = NULL; /* bridge sentinel */
	st->counter_id  = cfg->counter_id;
	st->be_data     = NULL;
	caps_out->flags = 0u; /* no HW_ALARM via bridge */
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

static alp_status_t br_us_to_ticks(alp_counter_backend_state_t *st, uint32_t us,
                                   uint32_t *ticks_out)
{
	(void)st;
	(void)us;
	*ticks_out = 0u;
	return ALP_ERR_NOSUPPORT; /* v0.3 adds CMD_COUNTER_GET_FREQ */
}

static alp_status_t br_set_alarm(alp_counter_backend_state_t *st, uint32_t ticks_from_now,
                                 struct alp_counter *owner)
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

ALP_BACKEND_REGISTER(counter, gd32_bridge,
                     {
                         .silicon_ref = "renesas:rzv2n:n44",
                         .vendor      = "renesas",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
