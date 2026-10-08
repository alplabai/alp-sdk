/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * RTC power-domain hook through the RV-3028 chip driver (#2784, U5).
 *
 * The RV-3028 is the primary RTC and the primary timed-wake source: it is
 * never powered down (KEEP_ALIVE).  The only quiesce action is CLKOUT low,
 * through rv3028c7_route_clkout(); its endurance guard skips the EEPROM
 * commit once CLKOUT already reads low.  Restore is a no-op by design: the
 * alarm / countdown-timer interrupt state belongs to the wake path, not here.
 */

#include <alp/chips/rv3028c7.h>
#include <alp/peripheral.h>

#include "som_power.h"
#include "som_power_chips.h"

static alp_status_t rtc_quiesce(void *ctx, bool rail_off)
{
	(void)rail_off;
	return rv3028c7_route_clkout((rv3028c7_t *)ctx, RV3028C7_CLKOUT_LOW);
}

static alp_status_t rtc_restore(void *ctx, bool rail_off, bool early)
{
	(void)ctx;
	(void)rail_off;
	(void)early;
	return ALP_OK;
}

static const alp_som_power_hooks_t _hooks = {
	.quiesce = rtc_quiesce,
	.restore = rtc_restore,
};

alp_status_t alp_som_power_bind_rv3028(rv3028c7_t *ctx)
{
	if (ctx == NULL) {
		return ALP_ERR_INVAL;
	}
	return alp_som_power_bind(ALP_POWER_DOMAIN_RTC, &_hooks, ctx);
}
