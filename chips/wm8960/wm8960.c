/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cirrus / Wolfson WM8960 codec driver (I²C config).
 * See <alp/chips/wm8960.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/wm8960.h"

#define WM8960_REG_RESET 0x0Fu

alp_status_t wm8960_init(wm8960_t *dev, alp_i2c_t *bus, uint8_t i2c_addr)
{
    if (dev == NULL || bus == NULL) return ALP_ERR_INVAL;
    if (i2c_addr == 0) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->bus         = bus;
    dev->addr        = i2c_addr;
    dev->initialised = true;
    return ALP_OK;
}

alp_status_t wm8960_write_reg(wm8960_t *dev, uint8_t reg, uint16_t val_9bit)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (reg > 0x7Fu) return ALP_ERR_INVAL;
    /* 16-bit on-wire: bits 15..9 = reg, bits 8..0 = val. */
    uint16_t word = (uint16_t)(((uint16_t)reg << 9) | (val_9bit & 0x01FFu));
    uint8_t  buf[2] = {(uint8_t)(word >> 8), (uint8_t)(word & 0xFFu)};
    return alp_i2c_write(dev->bus, dev->addr, buf, sizeof(buf));
}

alp_status_t wm8960_soft_reset(wm8960_t *dev)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    return wm8960_write_reg(dev, WM8960_REG_RESET, 0x000u);
}

void wm8960_deinit(wm8960_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
}
