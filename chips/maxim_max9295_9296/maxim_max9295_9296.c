/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ADI (Maxim) MAX9295A + MAX9296A GMSL2 SerDes pair driver.
 * See <alp/chips/maxim_max9295_9296.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/maxim_max9295_9296.h"

#define MAX929x_REG_CTRL0 0x0010u

static alp_status_t gmsl2_read(alp_i2c_t *bus, uint8_t addr, uint16_t reg, uint8_t *val)
{
    uint8_t addr_be[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return alp_i2c_write_read(bus, addr, addr_be, sizeof(addr_be), val, 1);
}

static alp_status_t gmsl2_write(alp_i2c_t *bus, uint8_t addr, uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val};
    return alp_i2c_write(bus, addr, buf, sizeof(buf));
}

alp_status_t maxim_gmsl2_init(maxim_gmsl2_t *dev,
                              alp_i2c_t     *bus,
                              uint8_t        ser_addr,
                              uint8_t        des_addr)
{
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (ser_addr == 0 || des_addr == 0) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->bus      = bus;
    dev->ser_addr = ser_addr;
    dev->des_addr = des_addr;

    uint8_t id = 0;
    alp_status_t s = gmsl2_read(bus, des_addr, MAX929x_REG_DEV_ID, &id);
    if (s != ALP_OK) return s;
    if (id != MAX9296_DEV_ID) return ALP_ERR_IO;

    s = gmsl2_read(bus, ser_addr, MAX929x_REG_DEV_ID, &id);
    if (s != ALP_OK) return s;
    if (id != MAX9295_DEV_ID) return ALP_ERR_IO;

    dev->initialised = true;
    return ALP_OK;
}

alp_status_t maxim_gmsl2_read_des_id(maxim_gmsl2_t *dev, uint8_t *id_out)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (id_out == NULL) return ALP_ERR_INVAL;
    return gmsl2_read(dev->bus, dev->des_addr, MAX929x_REG_DEV_ID, id_out);
}

alp_status_t maxim_gmsl2_read_ser_id(maxim_gmsl2_t *dev, uint8_t *id_out)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (id_out == NULL) return ALP_ERR_INVAL;
    return gmsl2_read(dev->bus, dev->ser_addr, MAX929x_REG_DEV_ID, id_out);
}

alp_status_t maxim_gmsl2_soft_reset(maxim_gmsl2_t *dev)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    alp_status_t s = gmsl2_write(dev->bus, dev->des_addr, MAX929x_REG_CTRL0, 0x40);
    if (s != ALP_OK) return s;
    return gmsl2_write(dev->bus, dev->ser_addr, MAX929x_REG_CTRL0, 0x40);
}

void maxim_gmsl2_deinit(maxim_gmsl2_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
}
