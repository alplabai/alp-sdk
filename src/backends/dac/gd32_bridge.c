/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * V2N DAC backend.  V2N's M33 has no direct DAC; the board routes
 * E1M `DAC0` + `DAC1` to the GD32 IO MCU's PA4 / PA6 pads, and the
 * SDK dispatches through the GD32G553 supervisor MCU using the
 * existing alp_z_v2n_supervisor_* mutex and the gd32g553_dac_*
 * host-driver functions.
 *
 * [UNTESTED] the gd32g553_dac_set / gd32g553_dac_get host driver is
 * paper-correct (matches the bridge wire protocol) but bench-
 * unverified.  The supervisor acquire/release brackets every host-
 * driver call exactly as src/backends/adc/gd32_bridge.c does.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/dac.h>
#include <alp/peripheral.h>

#include "dac_ops.h"

/* Internal SDK headers — NOT customer-facing.  Provide:
 *   alp_z_v2n_supervisor_acquire / _release  (via v2n_supervisor.h)
 *   gd32g553_dac_set / gd32g553_dac_get       (via chips/gd32g553.h)
 */
#include "v2n_supervisor.h"
#include <alp/chips/gd32g553.h>

typedef struct gd32_bridge_state {
	uint8_t  channel;
	uint16_t last_mv;
	bool     in_use;
} gd32_bridge_state_t;

#ifndef CONFIG_ALP_SDK_MAX_DAC_HANDLES
#define CONFIG_ALP_SDK_MAX_DAC_HANDLES 2
#endif

static gd32_bridge_state_t _state_pool[CONFIG_ALP_SDK_MAX_DAC_HANDLES];

static gd32_bridge_state_t *_alloc_state(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_DAC_HANDLES; ++i) {
		if (!_state_pool[i].in_use) {
			memset(&_state_pool[i], 0, sizeof(_state_pool[i]));
			_state_pool[i].in_use = true;
			return &_state_pool[i];
		}
	}
	return NULL;
}

static void _free_state(gd32_bridge_state_t *s)
{
	s->in_use = false;
}

static alp_status_t
gd32_open(const alp_dac_config_t *cfg, alp_dac_backend_state_t *st, alp_capabilities_t *caps_out)
{
	/* E1M reserves 2 DAC channels (E1M_DAC_COUNT); the bridge
     * routes DAC0/DAC1 to the GD32's PA4 / PA6 pads. */
	if (cfg->channel_id >= 2u) {
		return ALP_ERR_INVAL;
	}

	/* V2N: dispatch through the GD32 supervisor singleton. */
	gd32g553_t  *ctx = NULL;
	alp_status_t s   = alp_z_v2n_supervisor_acquire(&ctx);
	if (s != ALP_OK) {
		return s;
	}
	s = gd32g553_dac_set(ctx, (uint8_t)cfg->channel_id, cfg->initial_mv);
	alp_z_v2n_supervisor_release();
	if (s != ALP_OK) {
		return s;
	}

	gd32_bridge_state_t *bs = _alloc_state();
	if (bs == NULL) {
		return ALP_ERR_NOMEM;
	}
	bs->channel = (uint8_t)cfg->channel_id;
	bs->last_mv = cfg->initial_mv;

	st->dev         = NULL; /* bridge sentinel */
	st->channel_id  = cfg->channel_id;
	st->be_data     = bs;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t gd32_write_mv(alp_dac_backend_state_t *st, uint16_t mv)
{
	gd32_bridge_state_t *bs = (gd32_bridge_state_t *)st->be_data;

	gd32g553_t  *ctx = NULL;
	alp_status_t s   = alp_z_v2n_supervisor_acquire(&ctx);
	if (s != ALP_OK) {
		return s;
	}
	s = gd32g553_dac_set(ctx, bs->channel, mv);
	alp_z_v2n_supervisor_release();
	if (s == ALP_OK) {
		bs->last_mv = mv;
	}
	return s;
}

static alp_status_t gd32_read_mv(alp_dac_backend_state_t *st, uint16_t *mv_out)
{
	gd32_bridge_state_t *bs = (gd32_bridge_state_t *)st->be_data;

	gd32g553_t  *ctx = NULL;
	alp_status_t s   = alp_z_v2n_supervisor_acquire(&ctx);
	if (s != ALP_OK) {
		return s;
	}
	s = gd32g553_dac_get(ctx, bs->channel, mv_out);
	alp_z_v2n_supervisor_release();
	if (s == ALP_OK) {
		bs->last_mv = *mv_out;
	}
	return s;
}

static void gd32_close(alp_dac_backend_state_t *st)
{
	if (st->be_data != NULL) {
		_free_state((gd32_bridge_state_t *)st->be_data);
		st->be_data = NULL;
	}
}

static const alp_dac_ops_t gd32_ops = {
	.open     = gd32_open,
	.write_mv = gd32_write_mv,
	.read_mv  = gd32_read_mv,
	.close    = gd32_close,
};

ALP_BACKEND_REGISTER(dac,
                     gd32_bridge,
                     {
                         .silicon_ref = "renesas:rzv2n:n44",
                         .vendor      = "renesas", /* SoC vendor, not bridge chip */
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &gd32_ops,
                         .probe       = NULL,
                     });
