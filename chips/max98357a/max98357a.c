/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Maxim MAX98357A class-D amp driver.
 * See <alp/chips/max98357a.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/max98357a.h"

alp_status_t max98357a_init(max98357a_t *dev, alp_gpio_t *sd_mode)
{
	if (dev == NULL || sd_mode == NULL) return ALP_ERR_INVAL;
	memset(dev, 0, sizeof(*dev));
	dev->sd_mode     = sd_mode;
	dev->initialised = true;
	/* Default: amp shut down so the I²S consumer can configure first. */
	return alp_gpio_write(sd_mode, false);
}

alp_status_t max98357a_set_enabled(max98357a_t *dev, bool enabled)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	return alp_gpio_write(dev->sd_mode, enabled);
}

void max98357a_deinit(max98357a_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
	dev->sd_mode     = NULL;
}
