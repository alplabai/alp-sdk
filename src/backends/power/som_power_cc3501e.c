/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * WIFI_BLE power-domain hook through the CC3501E host driver (#2784, U5).
 *
 * Quiesce (AUTO): nRESET low.  The supply stays up, WIFI_EN is not touched.
 * Restore: cc3501e_hard_reset() -- 50 ms nRESET pulse, rails up, then the
 * blind boot settle.  NEVER cc3501e_reset(): that one drops WIFI_EN for 50 ms
 * (a supply cold cycle), which on an activated unit
 * (vendor_sbl_container_enable=1) may never relaunch the firmware.
 *
 * RAIL_OFF (opt-in, gated in the registry on
 * CONFIG_ALP_SDK_SOM_PD_WIFI_RAIL_OFF): cc3501e_power_off().  Its way back is
 * WIFI_EN high and then the same nRESET-only hard reset.
 */

#include <stdbool.h>

#include <alp/chips/cc3501e.h>
#include <alp/peripheral.h>

#include "som_power.h"
#include "som_power_chips.h"

#define CC3501E_RAIL_UP_MS 20u

static struct {
	cc3501e_t *fw;
	bool       was_initialised;
} _cc;

static alp_status_t cc_quiesce(void *ctx, bool rail_off)
{
	(void)ctx;
	cc3501e_t *fw = _cc.fw;
	if (fw == NULL || fw->reset_pin == NULL) {
		return ALP_ERR_NOT_READY;
	}
	_cc.was_initialised = fw->initialised;
	if (rail_off) {
		/* nRESET low first, then the supply; clears fw->initialised. */
		return cc3501e_power_off(fw);
	}
	alp_status_t s = alp_gpio_write(fw->reset_pin, false);
	if (s != ALP_OK) {
		return s;
	}
	/* Every later call now fails NOT_READY instead of clocking frames at a
	 * chip held in reset and burning a full timeout each. */
	fw->initialised = false;
	return ALP_OK;
}

static alp_status_t cc_restore(void *ctx, bool rail_off, bool early)
{
	(void)ctx;
	(void)early; /* the hard reset's blind settle is seconds; never bound at cold boot */
	cc3501e_t *fw = _cc.fw;
	if (fw == NULL) {
		return ALP_ERR_NOT_READY;
	}
	if (rail_off && fw->enable_pin != NULL) {
		alp_status_t s = alp_gpio_write(fw->enable_pin, true);
		if (s != ALP_OK) {
			return s;
		}
		alp_delay_ms(CC3501E_RAIL_UP_MS);
	}
	alp_status_t s = cc3501e_hard_reset(fw);
	if (s == ALP_OK) {
		fw->initialised = _cc.was_initialised;
	}
	return s;
}

static const alp_som_power_hooks_t _hooks = {
	.quiesce = cc_quiesce,
	.restore = cc_restore,
};

alp_status_t alp_som_power_bind_cc3501e(cc3501e_t *fw)
{
	if (fw == NULL) {
		return ALP_ERR_INVAL;
	}
	alp_status_t s = alp_som_power_bind(ALP_POWER_DOMAIN_WIFI_BLE, &_hooks, fw);
	if (s == ALP_OK) {
		_cc.fw = fw;
	}
	return s;
}
