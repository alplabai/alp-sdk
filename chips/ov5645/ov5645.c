/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * OmniVision OV5645 5 MP MIPI CSI-2 image sensor driver (SCCB side).
 * See <alp/chips/ov5645.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/ov5645.h"

#define OV5645_REG_SOFT_RESET 0x3008u /* bit 7 = system soft reset */

/* 16-bit register-address SCCB read. */
static alp_status_t ov5645_read_reg(ov5645_t *dev, uint16_t reg, uint8_t *val)
{
    uint8_t addr_be[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return alp_i2c_write_read(dev->bus, dev->addr, addr_be, sizeof(addr_be), val, 1);
}

static alp_status_t ov5645_write_reg(ov5645_t *dev, uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val};
    return alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
}

alp_status_t ov5645_init(ov5645_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (i2c_addr == 0) return ALP_ERR_INVAL;

    memset(dev, 0, sizeof(*dev));
    dev->bus  = bus;
    dev->addr = i2c_addr;

    uint8_t hi = 0, lo = 0;
    alp_status_t s = ov5645_read_reg(dev, OV5645_REG_CHIP_ID_HI, &hi);
    if (s != ALP_OK) return s;
    s = ov5645_read_reg(dev, OV5645_REG_CHIP_ID_LO, &lo);
    if (s != ALP_OK) return s;
    uint16_t id = ((uint16_t)hi << 8) | lo;
    if (id != OV5645_CHIP_ID) return ALP_ERR_IO;

    dev->res         = OV5645_RES_1080P;
    dev->lanes       = OV5645_LANES_2;
    dev->initialised = true;
    return ALP_OK;
}

alp_status_t ov5645_read_id(ov5645_t *dev, uint16_t *id_out)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (id_out == NULL) return ALP_ERR_INVAL;

    uint8_t hi = 0, lo = 0;
    alp_status_t s = ov5645_read_reg(dev, OV5645_REG_CHIP_ID_HI, &hi);
    if (s != ALP_OK) return s;
    s = ov5645_read_reg(dev, OV5645_REG_CHIP_ID_LO, &lo);
    if (s != ALP_OK) return s;
    *id_out = ((uint16_t)hi << 8) | lo;
    return ALP_OK;
}

alp_status_t ov5645_soft_reset(ov5645_t *dev)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    return ov5645_write_reg(dev, OV5645_REG_SOFT_RESET, 0x80);
}

alp_status_t ov5645_set_resolution(ov5645_t *dev, ov5645_resolution_t res)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if ((int)res < 0 || (int)res > (int)OV5645_RES_5MP) return ALP_ERR_INVAL;
    dev->res = res;
    return ALP_ERR_NOSUPPORT; /* Vendor register table pending. */
}

alp_status_t ov5645_set_lanes(ov5645_t *dev, ov5645_lanes_t lanes)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (lanes != OV5645_LANES_1 && lanes != OV5645_LANES_2) return ALP_ERR_INVAL;
    dev->lanes = lanes;
    return ALP_ERR_NOSUPPORT;
}

void ov5645_deinit(ov5645_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
}
