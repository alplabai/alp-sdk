/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * OmniVision OV9281 1 MP global-shutter MIPI CSI-2 driver (SCCB).
 * See <alp/chips/ov9281.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/ov9281.h"

#define OV9281_REG_SW_RESET 0x0103u

static alp_status_t ov9281_read_reg(ov9281_t *dev, uint16_t reg, uint8_t *val)
{
    uint8_t addr_be[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return alp_i2c_write_read(dev->bus, dev->addr, addr_be, sizeof(addr_be), val, 1);
}

static alp_status_t ov9281_write_reg(ov9281_t *dev, uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val};
    return alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
}

alp_status_t ov9281_init(ov9281_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (i2c_addr == 0) return ALP_ERR_INVAL;

    memset(dev, 0, sizeof(*dev));
    dev->bus  = bus;
    dev->addr = i2c_addr;

    uint8_t hi = 0, lo = 0;
    alp_status_t s = ov9281_read_reg(dev, OV9281_REG_CHIP_ID_HI, &hi);
    if (s != ALP_OK) return s;
    s = ov9281_read_reg(dev, OV9281_REG_CHIP_ID_LO, &lo);
    if (s != ALP_OK) return s;
    if ((((uint16_t)hi << 8) | lo) != OV9281_CHIP_ID) return ALP_ERR_IO;

    dev->initialised = true;
    return ALP_OK;
}

alp_status_t ov9281_read_id(ov9281_t *dev, uint16_t *id_out)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (id_out == NULL) return ALP_ERR_INVAL;

    uint8_t hi = 0, lo = 0;
    alp_status_t s = ov9281_read_reg(dev, OV9281_REG_CHIP_ID_HI, &hi);
    if (s != ALP_OK) return s;
    s = ov9281_read_reg(dev, OV9281_REG_CHIP_ID_LO, &lo);
    if (s != ALP_OK) return s;
    *id_out = ((uint16_t)hi << 8) | lo;
    return ALP_OK;
}

alp_status_t ov9281_soft_reset(ov9281_t *dev)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    return ov9281_write_reg(dev, OV9281_REG_SW_RESET, 0x01);
}

void ov9281_deinit(ov9281_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
}
