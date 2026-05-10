/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * spi-loopback — exercise alp_spi_open / alp_spi_transceive on
 * ALP_E1M_SPI0 with no CS GPIO (sentinel ALP_SPI_NO_CS = ~0u).
 */

#include <stdio.h>

#include "alp/peripheral.h"
#include "alp/e1m_pinout.h"

#define ALP_SPI_NO_CS  0xFFFFFFFFu

int main(void) {
    printf("[spi] open ALP_E1M_SPI0 @ 1 MHz mode 0\n");

    alp_spi_t *bus = alp_spi_open(&(alp_spi_config_t){
        .bus_id        = ALP_E1M_SPI0,
        .freq_hz       = 1000000,
        .mode          = ALP_SPI_MODE_0,
        .bits_per_word = 8,
        .cs_pin_id     = ALP_SPI_NO_CS,
    });
    if (bus == NULL) {
        printf("[spi] open failed: alp_last_error=%d\n",
               (int)alp_last_error());
        printf("[spi] done\n");
        return 0;
    }

    uint8_t tx[4] = {0xAA, 0x55, 0x12, 0x34};
    uint8_t rx[4] = {0};
    alp_status_t s = alp_spi_transceive(bus, tx, rx, sizeof tx);
    printf("[spi] transceive -> status=%d  rx={%02x %02x %02x %02x}\n",
           (int)s, rx[0], rx[1], rx[2], rx[3]);

    alp_spi_close(bus);
    printf("[spi] done\n");
    return 0;
}
