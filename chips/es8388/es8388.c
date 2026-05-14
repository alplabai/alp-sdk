/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Everest Semi ES8388 codec driver (I²C control).
 * See <alp/chips/es8388.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/es8388.h"

#define ES8388_REG_CHIP_CTRL1 0x00u

alp_status_t es8388_init(es8388_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (i2c_addr == 0) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->bus         = bus;
    dev->addr        = i2c_addr;
    dev->initialised = true;
    return ALP_OK;
}

alp_status_t es8388_read_reg(es8388_t *dev, uint8_t reg, uint8_t *val)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (val == NULL) return ALP_ERR_INVAL;
    return alp_i2c_write_read(dev->bus, dev->addr, &reg, 1, val, 1);
}

alp_status_t es8388_write_reg(es8388_t *dev, uint8_t reg, uint8_t val)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    uint8_t buf[2] = {reg, val};
    return alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
}

alp_status_t es8388_soft_reset(es8388_t *dev)
{
    /* CHIP_CTRL1 bit 7 = MCLK_DIS, bit 0 = soft reset (auto-clears). */
    return es8388_write_reg(dev, ES8388_REG_CHIP_CTRL1, 0x80);
}

void es8388_deinit(es8388_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
}
