/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * RAiO RA8875 LCD controller driver.  See <alp/chips/ra8875.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/ra8875.h"
#include "alp/peripheral.h"

static alp_status_t ra8875_spi_w2(ra8875_t *dev, uint8_t b0, uint8_t b1)
{
    uint8_t buf[2] = {b0, b1};
    return alp_spi_write(dev->bus, buf, sizeof(buf));
}

alp_status_t ra8875_write_reg(ra8875_t *dev, uint8_t reg, uint8_t val)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    alp_status_t s = ra8875_spi_w2(dev, RA8875_CMD_WRITE, reg);
    if (s != ALP_OK) return s;
    return ra8875_spi_w2(dev, RA8875_DATA_WRITE, val);
}

alp_status_t ra8875_read_reg(ra8875_t *dev, uint8_t reg, uint8_t *val)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (val == NULL) return ALP_ERR_INVAL;
    alp_status_t s = ra8875_spi_w2(dev, RA8875_CMD_WRITE, reg);
    if (s != ALP_OK) return s;
    uint8_t tx[2] = {RA8875_DATA_READ, 0x00};
    uint8_t rx[2] = {0};
    s             = alp_spi_transceive(dev->bus, tx, rx, sizeof(tx));
    if (s != ALP_OK) return s;
    *val = rx[1];
    return ALP_OK;
}

alp_status_t ra8875_init(ra8875_t *dev, alp_spi_t *spi, alp_gpio_t *reset)
{
    if (dev == NULL || spi == NULL) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->bus   = spi;
    dev->reset = reset;

    if (reset != NULL) {
        (void)alp_gpio_write(reset, false);
        alp_delay_us(100000);
        (void)alp_gpio_write(reset, true);
        alp_delay_us(300000);
    }

    dev->initialised = true;

    /* Set PWRR display-on (bit 7) -- panel-init table is [stub-impl] still. */
    alp_status_t s = ra8875_write_reg(dev, RA8875_REG_PWRR, 0x80);
    if (s != ALP_OK) {
        dev->initialised = false;
        return s;
    }
    return ALP_OK;
}

alp_status_t ra8875_soft_reset(ra8875_t *dev)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    return ra8875_write_reg(dev, RA8875_REG_PWRR, 0x01);
}

void ra8875_deinit(ra8875_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
    dev->reset       = NULL;
}
