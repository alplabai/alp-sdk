/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright 2026 Alp Lab AB
 *
 * EXT_FLASH power-domain hook through flash_ospi_alif (#2784, U5).
 *
 * The driver exposes no power-management or deep-power-down hook, so the
 * action is the registry's reset-hold on OSPI1_RESETn (P15_7).  What the
 * driver needs to hear about is the consequence: the reset returns the part to
 * its power-on 1-1-1 framing, so flash_ospi_alif_reset_notify() makes it drop
 * its "part is in Octal DDR" state before the next access.
 */

#include <zephyr/device.h>
#include <zephyr/drivers/flash/flash_ospi_alif.h>
#include <zephyr/init.h>

#include <alp/peripheral.h>

#include "som_power.h"
#include "som_power_chips.h"

static alp_status_t flash_quiesce(void *ctx, bool rail_off)
{
	alp_status_t s = alp_som_power_pin_quiesce(ALP_POWER_DOMAIN_EXT_FLASH, rail_off);
	if (s == ALP_OK) {
		(void)flash_ospi_alif_reset_notify((const struct device *)ctx);
	}
	return s;
}

static alp_status_t flash_restore(void *ctx, bool rail_off, bool early)
{
	alp_status_t s = alp_som_power_pin_restore(ALP_POWER_DOMAIN_EXT_FLASH, rail_off, early);
	if (s == ALP_OK) {
		(void)flash_ospi_alif_reset_notify((const struct device *)ctx);
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
