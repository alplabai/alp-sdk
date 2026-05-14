/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * TI TLV320AIC3204 codec driver (I²C control).
 * See <alp/chips/tlv320aic3204.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/tlv320aic3204.h"

#define TLV320AIC3204_REG_SOFT_RESET 0x01u

alp_status_t tlv320aic3204_init(tlv320aic3204_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (i2c_addr == 0) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->bus          = bus;
    dev->addr         = i2c_addr;
    dev->current_page = 0xFFu; /* invalidate so first select forces a write. */
    dev->initialised  = true;
    return ALP_OK;
}

alp_status_t tlv320aic3204_select_page(tlv320aic3204_t *dev, uint8_t page)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (dev->current_page == page) return ALP_OK;
    uint8_t buf[2] = {TLV320AIC3204_REG_PAGE_SELECT, page};
    alp_status_t s = alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
    if (s != ALP_OK) return s;
    dev->current_page = page;
    return ALP_OK;
}

alp_status_t tlv320aic3204_read_reg(tlv320aic3204_t *dev, uint8_t reg, uint8_t *val)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (val == NULL) return ALP_ERR_INVAL;
    return alp_i2c_write_read(dev->bus, dev->addr, &reg, 1, val, 1);
}

alp_status_t tlv320aic3204_write_reg(tlv320aic3204_t *dev, uint8_t reg, uint8_t val)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    uint8_t buf[2] = {reg, val};
    return alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
}

alp_status_t tlv320aic3204_soft_reset(tlv320aic3204_t *dev)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    alp_status_t s = tlv320aic3204_select_page(dev, 0);
    if (s != ALP_OK) return s;
    return tlv320aic3204_write_reg(dev, TLV320AIC3204_REG_SOFT_RESET, 0x01);
}

void tlv320aic3204_deinit(tlv320aic3204_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
}
