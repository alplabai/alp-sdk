/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Semtech SX1262 LoRa transceiver driver (SPI).
 * See <alp/chips/semtech_sx1262.h>.
 */

#include <string.h>
#include <stdint.h>

#include "alp/chips/semtech_sx1262.h"

alp_status_t semtech_sx1262_init(semtech_sx1262_t *dev,
                                 alp_spi_t        *spi,
                                 alp_gpio_t       *nreset,
                                 alp_gpio_t       *busy)
{
    if (dev == NULL || spi == NULL) return ALP_ERR_INVAL;
    memset(dev, 0, sizeof(*dev));
    dev->bus         = spi;
    dev->nreset      = nreset;
    dev->busy        = busy;
    dev->initialised = true;
    return ALP_OK;
}

alp_status_t semtech_sx1262_hw_reset(semtech_sx1262_t *dev)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (dev->nreset == NULL) return ALP_ERR_NOSUPPORT;
    alp_status_t s = alp_gpio_write(dev->nreset, false);
    if (s != ALP_OK) return s;
    alp_delay_us(200);
    s = alp_gpio_write(dev->nreset, true);
    if (s != ALP_OK) return s;
    alp_delay_us(10000);
    return ALP_OK;
}

alp_status_t semtech_sx1262_wait_busy(semtech_sx1262_t *dev, uint32_t timeout_ms)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (dev->busy == NULL) {
        alp_delay_us(1000);
        return ALP_OK;
    }
    uint32_t waited_ms = 0;
    while (waited_ms < timeout_ms) {
        bool         level = true;
        alp_status_t s     = alp_gpio_read(dev->busy, &level);
        if (s != ALP_OK) return s;
        if (!level) return ALP_OK;
        alp_delay_us(1000);
        waited_ms++;
    }
    return ALP_ERR_TIMEOUT;
}

alp_status_t semtech_sx1262_get_status(semtech_sx1262_t *dev, uint8_t *status_out)
{
    if (dev == NULL || !dev->initialised) return ALP_ERR_NOT_READY;
    if (status_out == NULL) return ALP_ERR_INVAL;
    uint8_t tx[2] = {SX1262_OPCODE_GET_STATUS, 0x00};
    uint8_t rx[2] = {0};
    alp_status_t s = alp_spi_transceive(dev->bus, tx, rx, sizeof(tx));
    if (s != ALP_OK) return s;
    *status_out = rx[1];
    return ALP_OK;
}

void semtech_sx1262_deinit(semtech_sx1262_t *dev)
{
    if (dev == NULL) return;
    dev->initialised = false;
    dev->bus         = NULL;
    dev->nreset      = NULL;
    dev->busy        = NULL;
}
