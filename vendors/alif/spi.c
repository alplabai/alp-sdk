/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bare-metal Alif Ensemble SPI wrapper for <alp/peripheral.h>.
 * See vendors/alif/i2c.c for the gating + scaffolding rationale.
 */

#include "alp/peripheral.h"

#if defined(ALP_HAS_ALIF_HAL)

#include <stddef.h>
#include <stdint.h>

#include "Driver_SPI.h"

extern ARM_DRIVER_SPI        Driver_SPI0;
extern ARM_DRIVER_SPI        Driver_SPI1;
extern ARM_DRIVER_SPI        Driver_SPI2;
extern ARM_DRIVER_SPI        Driver_SPI3;

static ARM_DRIVER_SPI *const alp_alif_spi_drivers[] = {
    &Driver_SPI0,
    &Driver_SPI1,
    &Driver_SPI2,
    &Driver_SPI3,
};

static alp_status_t alif_to_alp(int32_t st)
{
    switch (st) {
    case ARM_DRIVER_OK:
        return ALP_OK;
    case ARM_DRIVER_ERROR_BUSY:
        return ALP_ERR_BUSY;
    case ARM_DRIVER_ERROR_TIMEOUT:
        return ALP_ERR_TIMEOUT;
    case ARM_DRIVER_ERROR_PARAMETER:
        return ALP_ERR_INVAL;
    case ARM_DRIVER_ERROR_UNSUPPORTED:
        return ALP_ERR_NOSUPPORT;
    default:
        return ALP_ERR_IO;
    }
}

alp_spi_t *alp_spi_open(const alp_spi_config_t *cfg)
{
    if (cfg == NULL) return NULL;
    if (cfg->bus_id >= sizeof(alp_alif_spi_drivers) / sizeof(alp_alif_spi_drivers[0])) {
        return NULL;
    }
    ARM_DRIVER_SPI *d = alp_alif_spi_drivers[cfg->bus_id];
    if (d == NULL) return NULL;
    if (d->Initialize(NULL) != ARM_DRIVER_OK) return NULL;
    if (d->PowerControl(ARM_POWER_FULL) != ARM_DRIVER_OK) return NULL;
    uint32_t cpol_cpha = ARM_SPI_CPOL0_CPHA0;
    switch (cfg->mode) {
    case ALP_SPI_MODE_1:
        cpol_cpha = ARM_SPI_CPOL0_CPHA1;
        break;
    case ALP_SPI_MODE_2:
        cpol_cpha = ARM_SPI_CPOL1_CPHA0;
        break;
    case ALP_SPI_MODE_3:
        cpol_cpha = ARM_SPI_CPOL1_CPHA1;
        break;
    default:
        cpol_cpha = ARM_SPI_CPOL0_CPHA0;
        break;
    }
    uint32_t mode = ARM_SPI_MODE_MASTER | cpol_cpha |
                    ARM_SPI_DATA_BITS(cfg->bits_per_word ? cfg->bits_per_word : 8);
    if (d->Control(mode, cfg->freq_hz) != ARM_DRIVER_OK) return NULL;
    return (alp_spi_t *)d;
}

alp_status_t alp_spi_transceive(alp_spi_t *bus, const uint8_t *tx, uint8_t *rx, size_t len)
{
    if (bus == NULL) return ALP_ERR_NOT_READY;
    return alif_to_alp(((ARM_DRIVER_SPI *)bus)->Transfer(tx, rx, (uint32_t)len));
}

alp_status_t alp_spi_write(alp_spi_t *bus, const uint8_t *tx, size_t len)
{
    if (bus == NULL) return ALP_ERR_NOT_READY;
    return alif_to_alp(((ARM_DRIVER_SPI *)bus)->Send(tx, (uint32_t)len));
}

alp_status_t alp_spi_read(alp_spi_t *bus, uint8_t *rx, size_t len)
{
    if (bus == NULL) return ALP_ERR_NOT_READY;
    return alif_to_alp(((ARM_DRIVER_SPI *)bus)->Receive(rx, (uint32_t)len));
}

void alp_spi_close(alp_spi_t *bus)
{
    if (bus == NULL) return;
    ARM_DRIVER_SPI *d = (ARM_DRIVER_SPI *)bus;
    (void)d->PowerControl(ARM_POWER_OFF);
    (void)d->Uninitialize();
}

#endif /* ALP_HAS_ALIF_HAL */
