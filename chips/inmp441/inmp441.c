/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * InvenSense INMP441 MEMS mic driver.  See <alp/chips/inmp441.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/inmp441.h"

alp_status_t inmp441_init(inmp441_t *dev, inmp441_channel_t channel)
{
	if (dev == NULL) return ALP_ERR_INVAL;
	if (channel != INMP441_CH_LEFT && channel != INMP441_CH_RIGHT) return ALP_ERR_INVAL;
	memset(dev, 0, sizeof(*dev));
	dev->channel     = channel;
	dev->initialised = true;
	return ALP_OK;
}

alp_status_t inmp441_get_channel(inmp441_t *dev, inmp441_channel_t *channel_out)
{
	if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
	if (channel_out == NULL) return ALP_ERR_INVAL;
	*channel_out = dev->channel;
	return ALP_OK;
}

void inmp441_deinit(inmp441_t *dev)
{
	if (dev == NULL) return;
	dev->initialised = false;
}
