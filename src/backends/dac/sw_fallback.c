/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software DAC fallback backend.  Registered (priority 0, "*") only
 * to keep the dac class section non-empty for the registry's
 * __start_/__stop_ linker bounds.  native_sim has no real DAC
 * controller, so open() succeeds with an empty capability set but
 * write_mv / read_mv return ALP_ERR_NOSUPPORT -- there is no analog
 * output to drive.
 *
 * @par Cost: ROM ~400 B, RAM 0 B (stateless; no device, no buffer).
 * @par Performance: O(1) per call; write/read short-circuit to
 *      NOSUPPORT.  For native_sim build/test only -- never use on
 *      production hardware.
 */

#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/dac.h>
#include <alp/peripheral.h>

#include "dac_ops.h"

static alp_status_t
sw_open(const alp_dac_config_t *cfg, alp_dac_backend_state_t *st, alp_capabilities_t *caps_out)
{
	(void)cfg;
	st->dev         = NULL;
	st->channel_id  = cfg->channel_id;
	st->be_data     = NULL;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t sw_write_mv(alp_dac_backend_state_t *st, uint16_t mv)
{
	(void)st;
	(void)mv;
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_read_mv(alp_dac_backend_state_t *st, uint16_t *mv_out)
{
	(void)st;
	(void)mv_out;
	return ALP_ERR_NOSUPPORT;
}

static const alp_dac_ops_t sw_ops = {
	.open     = sw_open,
	.write_mv = sw_write_mv,
	.read_mv  = sw_read_mv,
	.close    = NULL,
};

ALP_BACKEND_ANCHOR_DEFINE(dac);
ALP_BACKEND_REGISTER(dac,
                     sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &sw_ops,
                         .probe       = NULL,
                     });
