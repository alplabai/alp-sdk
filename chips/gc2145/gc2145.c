/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * GalaxyCore GC2145 2 MP DVP camera driver (SCCB).
 * See <alp/chips/gc2145.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/gc2145.h"

static alp_status_t gc2145_read_reg(gc2145_t *dev, uint8_t reg, uint8_t *val)
{
    return alp_i2c_write_read(dev->bus, dev->addr, &reg, 1, val, 1);
}

static alp_status_t gc2145_write_reg(gc2145_t *dev, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
}

alp_status_t gc2145_init(gc2145_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (i2c_addr == 0) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->bus  = bus;
    dev->addr = i2c_addr;

    uint8_t hi = 0, lo = 0;
    alp_status_t s = gc2145_read_reg(dev, GC2145_REG_PID_HI, &hi);
    if (s != ALP_OK) return s;
    s = gc2145_read_reg(dev, GC2145_REG_PID_LO, &lo);
    if (s != ALP_OK) return s;
    if ((((uint16_t)hi << 8) | lo) != GC2145_CHIP_ID) return ALP_ERR_IO;

    dev->initialised = true;
    return ALP_OK;
}

alp_status_t gc2145_read_id(gc2145_t *dev, uint16_t *id_out)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (id_out == NULL) return ALP_ERR_INVAL;
    uint8_t hi = 0, lo = 0;
    alp_status_t s = gc2145_read_reg(dev, GC2145_REG_PID_HI, &hi);
    if (s != ALP_OK) return s;
    s = gc2145_read_reg(dev, GC2145_REG_PID_LO, &lo);
    if (s != ALP_OK) return s;
    *id_out = ((uint16_t)hi << 8) | lo;
    return ALP_OK;
}

alp_status_t gc2145_soft_reset(gc2145_t *dev)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    return gc2145_write_reg(dev, 0xFE, 0x80);
}

void gc2145_deinit(gc2145_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
}
