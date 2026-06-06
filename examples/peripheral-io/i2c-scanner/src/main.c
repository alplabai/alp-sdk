/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * i2c-scanner — open the EVK sensor bus and probe every 7-bit
 * address.  The canonical "any device on this bus?" pattern.
 */

#include <stdio.h>

#include "alp/peripheral.h"

/* BOARD_I2C_SENSORS is a portable cross-EVK alias from <alp/board.h>:
 *   E1M EVK  -> EVK_I2C_BUS_SENSORS  -> E1M_I2C0
 *   E1M-X EVK -> XEVK_I2C_BUS_SENSORS -> E1M_X_I2C0
 * Rebind it in board.yaml `pins:` to port to another board. */
#include "alp/board.h"

int main(void)
{
    printf("[i2c] open BOARD_I2C_SENSORS @ 100 kHz\n");

    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = BOARD_I2C_SENSORS, /* E1M EVK: E1M_I2C0; E1M-X EVK: E1M_X_I2C0 */
        .bitrate_hz = 100000,
    });
    if (bus == NULL) {
        printf("[i2c] open failed: alp_last_error=%d\n", (int)alp_last_error());
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
