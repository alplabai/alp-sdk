/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Vishay VEML7700 ALS driver.  See <alp/chips/veml7700.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/veml7700.h"

static alp_status_t veml7700_write16(veml7700_t *dev, uint8_t reg, uint16_t val)
{
    /* All VEML7700 registers are 16-bit little-endian on the wire. */
    uint8_t buf[3] = {reg, (uint8_t)(val & 0xFFu), (uint8_t)(val >> 8)};
    return alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
}

static alp_status_t veml7700_read16(veml7700_t *dev, uint8_t reg, uint16_t *val)
{
    uint8_t buf[2] = {0};
    alp_status_t s = alp_i2c_write_read(dev->bus, dev->addr, &reg, 1, buf, sizeof(buf));
    if (s != ALP_OK) return s;
    *val = (uint16_t)((uint16_t)buf[0] | ((uint16_t)buf[1] << 8));
    return ALP_OK;
}

alp_status_t veml7700_init(veml7700_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (i2c_addr == 0) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->bus  = bus;
    dev->addr = i2c_addr;

    /* CONF[0]=SD (shutdown). Clear to power on; gain/IT default 0. */
    alp_status_t s = veml7700_write16(dev, VEML7700_REG_CONF, 0x0000u);
    if (s != ALP_OK) return s;

    dev->initialised = true;
    return ALP_OK;
}

alp_status_t veml7700_read_als(veml7700_t *dev, uint16_t *als_out)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (als_out == NULL) return ALP_ERR_INVAL;
    return veml7700_read16(dev, VEML7700_REG_ALS, als_out);
}

void veml7700_deinit(veml7700_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
}
