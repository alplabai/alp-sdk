/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Semtech SX1276 LoRa driver.  See <alp/chips/semtech_sx1276.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/semtech_sx1276.h"

#define SX1276_WRITE_BIT 0x80u

alp_status_t semtech_sx1276_init(semtech_sx1276_t *dev,
                                 alp_spi_t        *spi,
                                 alp_gpio_t       *nreset)
{
    if (dev == NULL || spi == NULL) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->bus         = spi;
    dev->nreset      = nreset;
    dev->initialised = true;
    return ALP_OK;
}

alp_status_t semtech_sx1276_hw_reset(semtech_sx1276_t *dev)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (dev->nreset == NULL) return ALP_ERR_NOSUPPORT;
    alp_status_t s = alp_gpio_write(dev->nreset, false);
    if (s != ALP_OK) return s;
    alp_delay_us(200);
    s = alp_gpio_write(dev->nreset, true);
    if (s != ALP_OK) return s;
    alp_delay_us(5000);
    return ALP_OK;
}

alp_status_t semtech_sx1276_read_reg(semtech_sx1276_t *dev, uint8_t reg, uint8_t *val)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (val == NULL) return ALP_ERR_INVAL;
    uint8_t tx[2] = {(uint8_t)(reg & 0x7Fu), 0x00};
    uint8_t rx[2] = {0};
    alp_status_t s = alp_spi_transceive(dev->bus, tx, rx, sizeof(tx));
    if (s != ALP_OK) return s;
    *val = rx[1];
    return ALP_OK;
}

alp_status_t semtech_sx1276_write_reg(semtech_sx1276_t *dev, uint8_t reg, uint8_t val)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    uint8_t buf[2] = {(uint8_t)(reg | SX1276_WRITE_BIT), val};
    return alp_spi_write(dev->bus, buf, sizeof(buf));
}

alp_status_t semtech_sx1276_read_version(semtech_sx1276_t *dev, uint8_t *ver_out)
{
    return semtech_sx1276_read_reg(dev, SX1276_REG_VERSION, ver_out);
}

void semtech_sx1276_deinit(semtech_sx1276_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
    dev->nreset      = NULL;
}
