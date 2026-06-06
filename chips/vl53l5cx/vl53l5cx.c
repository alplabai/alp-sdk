/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ST VL53L5CX multi-zone ToF driver (I²C).  See <alp/chips/vl53l5cx.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/vl53l5cx.h"

static alp_status_t vl53l5cx_read16(vl53l5cx_t *dev, uint16_t reg, uint8_t *val)
{
    uint8_t addr_be[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return alp_i2c_write_read(dev->bus, dev->addr, addr_be, sizeof(addr_be), val, 1);
}

static alp_status_t vl53l5cx_write16(vl53l5cx_t *dev, uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val};
    return alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
}

alp_status_t vl53l5cx_init(vl53l5cx_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (i2c_addr == 0) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->bus  = bus;
    dev->addr = i2c_addr;

    /* Probe by reading DEVICE_ID; pre-fw-boot it reads 0xF0. */
    uint8_t      id = 0;
    alp_status_t s  = vl53l5cx_read16(dev, VL53L5CX_REG_DEVICE_ID, &id);
    if (s != ALP_OK) return s;
    if (id == 0 || id == 0xFF) return ALP_ERR_IO;

    dev->initialised = true;
    return ALP_OK;
}

alp_status_t vl53l5cx_read_id(vl53l5cx_t *dev, uint8_t *id_out)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (id_out == NULL) return ALP_ERR_INVAL;
    return vl53l5cx_read16(dev, VL53L5CX_REG_DEVICE_ID, id_out);
}

alp_status_t vl53l5cx_soft_reset(vl53l5cx_t *dev)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    return vl53l5cx_write16(dev, 0x0114, 0x00);
}

void vl53l5cx_deinit(vl53l5cx_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
}
