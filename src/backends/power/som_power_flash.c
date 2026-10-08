/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * EXT_FLASH power-domain hook through flash_ospi_alif (#2784, U5).
 *
 * The driver exposes no power-management or deep-power-down hook, so the
 * action is the registry's reset-hold on OSPI1_RESETn (P15_7).  Resetting the
 * part must never land on a write or erase in progress (a torn sector), so:
 *
 *   quiesce  flash_ospi_alif_suspend(): take the driver lock and wait for WIP to
 *            clear.  The lock stays held, so no access can start during the
 *            hold.  Only then is RESETn asserted.  A transfer still in flight
 *            past the wait fails the quiesce with ALP_ERR_BUSY, RESETn untouched.
 *   restore  release RESETn, then flash_ospi_alif_resume(): the driver forgets
 *            its Octal DDR switch (the part is back in 1-1-1) and unlocks.
 *
 * The same thread must quiesce and restore (the driver lock is a mutex).
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/flash/flash_ospi_alif.h>
#include <zephyr/init.h>

#include <alp/peripheral.h>

#include "som_power.h"
#include "som_power_chips.h"

/* How long to wait for a transfer already in flight before refusing the quiesce. */
#define FLASH_SUSPEND_WAIT_MS 5000u

static bool _suspended;

static alp_status_t flash_quiesce(void *ctx, bool rail_off)
{
	const struct device *dev = ctx;
	int                  rc  = flash_ospi_alif_suspend(dev, FLASH_SUSPEND_WAIT_MS);

	if (rc != 0) {
		return (rc == -EBUSY) ? ALP_ERR_BUSY : ALP_ERR_IO;
	}
	_suspended     = true;
	alp_status_t s = alp_som_power_pin_quiesce(ALP_POWER_DOMAIN_EXT_FLASH, rail_off);
	if (s != ALP_OK) {
		/* Reset was not (fully) asserted: give the driver back. */
		(void)alp_som_power_pin_restore(ALP_POWER_DOMAIN_EXT_FLASH, rail_off, false, true);
		(void)flash_ospi_alif_resume(dev);
		_suspended = false;
	}
	return s;
}

static alp_status_t flash_restore(void *ctx, bool rail_off, bool early)
{
	alp_status_t s = alp_som_power_pin_restore(ALP_POWER_DOMAIN_EXT_FLASH, rail_off, early, true);

	if (_suspended) {
		/* Unlock even when the pin restore failed: a stuck lock would hang every
		 * later flash access instead of reporting the error. */
		(void)flash_ospi_alif_resume((const struct device *)ctx);
		_suspended = false;
	}
	return s;
}

static const alp_som_power_hooks_t _hooks = {
	.quiesce = flash_quiesce,
	.restore = flash_restore,
};

alp_status_t alp_som_power_bind_flash(const struct device *ospi)
{
	if (ospi == NULL) {
		return ALP_ERR_INVAL;
	}
	return alp_som_power_bind(ALP_POWER_DOMAIN_EXT_FLASH, &_hooks, (void *)ospi);
}

/* After the cold-boot restore (priority 0) so that path stays on the pin
 * action alone; the driver itself initialises later still. */
static int flash_default_bind(void)
{
#if DT_HAS_COMPAT_STATUS_OKAY(snps_designware_ospi)
	const struct device *ospi = DEVICE_DT_GET_ANY(snps_designware_ospi);
	if (ospi != NULL) {
		(void)alp_som_power_bind_flash(ospi);
	}
#endif
	return 0;
}

SYS_INIT(flash_default_bind, POST_KERNEL, 1);
