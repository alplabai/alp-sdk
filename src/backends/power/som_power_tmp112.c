/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * TEMP_SENSOR power-domain hook through the TMP112 chip driver (#2784, U5):
 * the shutdown bit, via tmp112_set_shutdown().
 */

#include <alp/chips/tmp112.h>
#include <alp/peripheral.h>

#include "som_power.h"
#include "som_power_chips.h"

static alp_status_t tmp_quiesce(void *ctx, bool rail_off)
{
	(void)rail_off;
	return tmp112_set_shutdown((tmp112_t *)ctx, true);
}

static alp_status_t tmp_restore(void *ctx, bool rail_off, bool early)
{
	(void)rail_off;
	(void)early;
	return tmp112_set_shutdown((tmp112_t *)ctx, false);
}

static const alp_som_power_hooks_t _hooks = {
	.quiesce = tmp_quiesce,
	.restore = tmp_restore,
};

alp_status_t alp_som_power_bind_tmp112(tmp112_t *ctx)
{
	if (ctx == NULL) {
		return ALP_ERR_INVAL;
	}
	return alp_som_power_bind(ALP_POWER_DOMAIN_TEMP_SENSOR, &_hooks, ctx);
}
