/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * InvenSense ICS-43434 MEMS mic driver.  See <alp/chips/ics_43434.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/ics_43434.h"

alp_status_t ics_43434_init(ics_43434_t *dev, ics_43434_channel_t channel)
{
    if (dev == NULL) return ALP_ERR_INVAL;
    if (channel != ICS_43434_CH_LEFT && channel != ICS_43434_CH_RIGHT) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->channel     = channel;
    dev->initialised = true;
    return ALP_OK;
}

alp_status_t ics_43434_get_channel(ics_43434_t *dev, ics_43434_channel_t *channel_out)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (channel_out == NULL) return ALP_ERR_INVAL;
    *channel_out = dev->channel;
    return ALP_OK;
}

void ics_43434_deinit(ics_43434_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
}
