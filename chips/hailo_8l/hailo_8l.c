/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hailo-8L 13 TOPS NPU (M.2 PCIe) host-side bring-up driver.
 * See <alp/chips/hailo_8l.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/hailo_8l.h"
#include "alp/peripheral.h"

alp_status_t hailo_8l_reset(hailo_8l_t *dev)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (dev->resetb == NULL) return ALP_ERR_NOSUPPORT;
    alp_status_t s = alp_gpio_write(dev->resetb, false);
    if (s != ALP_OK) return s;
    alp_delay_us(100000);
    return alp_gpio_write(dev->resetb, true);
}

alp_status_t hailo_8l_init(hailo_8l_t *dev,
                           alp_gpio_t *resetb,
                           alp_gpio_t *pe_wake)
{
    if (dev == NULL || resetb == NULL) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->resetb      = resetb;
    dev->pe_wake     = pe_wake;
    dev->initialised = true;
    return hailo_8l_reset(dev);
}

alp_status_t hailo_8l_read_wake(hailo_8l_t *dev, bool *wake_asserted)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (wake_asserted == NULL) return ALP_ERR_INVAL;
    if (dev->pe_wake == NULL) {
        *wake_asserted = false;
        return ALP_ERR_NOSUPPORT;
    }
    bool         level = false;
    alp_status_t s     = alp_gpio_read(dev->pe_wake, &level);
    if (s != ALP_OK) return s;
    *wake_asserted = !level; /* WAKE# is active low. */
    return ALP_OK;
}

void hailo_8l_deinit(hailo_8l_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->resetb      = NULL;
    dev->pe_wake     = NULL;
}
