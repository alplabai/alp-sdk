/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * TI DS90UB953-Q1 + DS90UB954-Q1 FPD-Link III SerDes pair driver.
 * See <alp/chips/ti_ds90ub953_954.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/ti_ds90ub953_954.h"

#define DS90UB_REG_RESET_CTL 0x01u

static alp_status_t ds90ub_read(alp_i2c_t *bus, uint8_t addr, uint8_t reg, uint8_t *val)
{
    return alp_i2c_write_read(bus, addr, &reg, 1, val, 1);
}

static alp_status_t ds90ub_write(alp_i2c_t *bus, uint8_t addr, uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    return alp_i2c_write(bus, addr, buf, sizeof(buf));
}

alp_status_t ti_ds90ub_init(ti_ds90ub_t *dev,
                            alp_i2c_t   *bus,
                            uint8_t      ser_addr,
                            uint8_t      des_addr)
{
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (ser_addr == 0 || des_addr == 0) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->bus      = bus;
    dev->ser_addr = ser_addr;
    dev->des_addr = des_addr;

    /* Probe both ends for liveness; non-zero device IDs only. */
    uint8_t id = 0;
    alp_status_t s = ds90ub_read(bus, des_addr, DS90UB954_REG_DEVICE_ID, &id);
    if (s != ALP_OK) return s;
    if (id == 0 || id == 0xFF) return ALP_ERR_IO;

    s = ds90ub_read(bus, ser_addr, DS90UB953_REG_DEVICE_ID, &id);
    if (s != ALP_OK) return s;
    if (id == 0 || id == 0xFF) return ALP_ERR_IO;

    dev->initialised = true;
    return ALP_OK;
}

alp_status_t ti_ds90ub_read_des_id(ti_ds90ub_t *dev, uint8_t *id_out)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (id_out == NULL) return ALP_ERR_INVAL;
    return ds90ub_read(dev->bus, dev->des_addr, DS90UB954_REG_DEVICE_ID, id_out);
}

alp_status_t ti_ds90ub_read_ser_id(ti_ds90ub_t *dev, uint8_t *id_out)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (id_out == NULL) return ALP_ERR_INVAL;
    return ds90ub_read(dev->bus, dev->ser_addr, DS90UB953_REG_DEVICE_ID, id_out);
}

alp_status_t ti_ds90ub_soft_reset(ti_ds90ub_t *dev)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    alp_status_t s = ds90ub_write(dev->bus, dev->des_addr, DS90UB_REG_RESET_CTL, 0x01);
    if (s != ALP_OK) return s;
    return ds90ub_write(dev->bus, dev->ser_addr, DS90UB_REG_RESET_CTL, 0x01);
}

void ti_ds90ub_deinit(ti_ds90ub_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
}
