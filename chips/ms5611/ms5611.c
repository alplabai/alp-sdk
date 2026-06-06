/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * TE Connectivity MS5611 barometer driver (I²C).
 * See <alp/chips/ms5611.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/ms5611.h"

static alp_status_t ms5611_read_prom(ms5611_t *dev, uint8_t idx, uint16_t *val)
{
    uint8_t cmd       = (uint8_t)(MS5611_CMD_PROM_BASE | (uint8_t)(idx << 1));
    uint8_t buf[2]    = {0};
    alp_status_t s = alp_i2c_write_read(dev->bus, dev->addr, &cmd, 1, buf, sizeof(buf));
    if (s != ALP_OK) return s;
    *val = ((uint16_t)buf[0] << 8) | buf[1];
    return ALP_OK;
}

alp_status_t ms5611_init(ms5611_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (i2c_addr == 0) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->bus  = bus;
    dev->addr = i2c_addr;

    /* Soft reset. */
    uint8_t cmd = MS5611_CMD_RESET;
    alp_status_t s = alp_i2c_write(bus, i2c_addr, &cmd, 1);
    if (s != ALP_OK) return s;
    alp_delay_us(3000);

    /* Read all eight calibration PROM words. */
    for (uint8_t i = 0; i < 8; i++) {
        s = ms5611_read_prom(dev, i, &dev->prom[i]);
        if (s != ALP_OK) return s;
    }

    dev->initialised = true;
    return ALP_OK;
}

alp_status_t ms5611_soft_reset(ms5611_t *dev)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    uint8_t cmd = MS5611_CMD_RESET;
    return alp_i2c_write(dev->bus, dev->addr, &cmd, 1);
}

alp_status_t ms5611_get_coefficient(ms5611_t *dev, uint8_t idx, uint16_t *coef_out)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (coef_out == NULL) return ALP_ERR_INVAL;
    if (idx >= 8) return ALP_ERR_INVAL;
    *coef_out = dev->prom[idx];
    return ALP_OK;
}

void ms5611_deinit(ms5611_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
}
