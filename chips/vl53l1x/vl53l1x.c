/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ST VL53L1X time-of-flight ranger driver (I²C).
 * See <alp/chips/vl53l1x.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/vl53l1x.h"

#define VL53L1X_REG_SOFT_RESET 0x0000u

static alp_status_t vl53l1x_read16(vl53l1x_t *dev, uint16_t reg, uint8_t *val)
{
    uint8_t addr_be[2] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF)};
    return alp_i2c_write_read(dev->bus, dev->addr, addr_be, sizeof(addr_be), val, 1);
}

static alp_status_t vl53l1x_write16(vl53l1x_t *dev, uint16_t reg, uint8_t val)
{
    uint8_t buf[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), val};
    return alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
}

alp_status_t vl53l1x_init(vl53l1x_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (i2c_addr == 0) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->bus  = bus;
    dev->addr = i2c_addr;

    uint8_t      id = 0;
    alp_status_t s  = vl53l1x_read16(dev, VL53L1X_REG_IDENTIFICATION_MODEL_ID, &id);
    if (s != ALP_OK) return s;
    if (id != VL53L1X_MODEL_ID) return ALP_ERR_IO;

    dev->initialised = true;
    return ALP_OK;
}

alp_status_t vl53l1x_read_id(vl53l1x_t *dev, uint8_t *id_out)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (id_out == NULL) return ALP_ERR_INVAL;
    return vl53l1x_read16(dev, VL53L1X_REG_IDENTIFICATION_MODEL_ID, id_out);
}

alp_status_t vl53l1x_soft_reset(vl53l1x_t *dev)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    return vl53l1x_write16(dev, VL53L1X_REG_SOFT_RESET, 0x00);
}

void vl53l1x_deinit(vl53l1x_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
}
