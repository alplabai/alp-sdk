/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * i2c-scanner — open ALP_E1M_I2C0 and probe every 7-bit address.
 * The canonical "any device on this bus?" pattern.
 */

#include <stdio.h>

#include "alp/peripheral.h"
#include "alp/e1m_pinout.h"

int main(void) {
    printf("[i2c] open ALP_E1M_I2C0 @ 100 kHz\n");

    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = ALP_E1M_I2C0,
        .bitrate_hz = 100000,
    });
    if (bus == NULL) {
        printf("[i2c] open failed: alp_last_error=%d\n",
               (int)alp_last_error());
        printf("[i2c] done\n");
        return 0;
    }

    int responders = 0;
    for (uint8_t addr = 0x08; addr < 0x78; addr++) {
        /* Zero-length write — the chip ACKs its address byte if
         * present, NACKs otherwise. */
        alp_status_t s = alp_i2c_write(bus, addr, NULL, 0);
        if (s == ALP_OK) {
            printf("[i2c] addr 0x%02x acked\n", addr);
            responders++;
        }
    }
    printf("[i2c] scan complete, %d responder(s)\n", responders);

    alp_i2c_close(bus);
    printf("[i2c] done\n");
    return 0;
}
