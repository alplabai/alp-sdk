/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * STMicroelectronics LPS22HB pressure sensor driver (I²C).
 * See <alp/chips/lps22hb.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/lps22hb.h"

#define LPS22HB_REG_CTRL_REG2 0x11u
#define LPS22HB_CTRL_REG2_SWRESET 0x04u

alp_status_t lps22hb_init(lps22hb_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (i2c_addr == 0) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->bus  = bus;
    dev->addr = i2c_addr;

    uint8_t reg = LPS22HB_REG_WHO_AM_I;
    uint8_t id  = 0;
    alp_status_t s = alp_i2c_write_read(bus, i2c_addr, &reg, 1, &id, 1);
    if (s != ALP_OK) return s;
    if (id != LPS22HB_WHO_AM_I) return ALP_ERR_IO;

    dev->initialised = true;
    return ALP_OK;
}

alp_status_t lps22hb_read_id(lps22hb_t *dev, uint8_t *id_out)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (id_out == NULL) return ALP_ERR_INVAL;
    uint8_t reg = LPS22HB_REG_WHO_AM_I;
    return alp_i2c_write_read(dev->bus, dev->addr, &reg, 1, id_out, 1);
}

alp_status_t lps22hb_soft_reset(lps22hb_t *dev)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    uint8_t buf[2] = {LPS22HB_REG_CTRL_REG2, LPS22HB_CTRL_REG2_SWRESET};
    return alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
}

void lps22hb_deinit(lps22hb_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
}
