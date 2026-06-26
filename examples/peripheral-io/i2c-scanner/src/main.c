/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * i2c-scanner — open the EVK sensor bus and probe every 7-bit
 * address.  The canonical "any device on this bus?" pattern.
 *
 * Runs on both EVKs: BOARD_I2C_SENSORS (from <alp/board.h>) resolves
 * to E1M_I2C0 on E1M EVK and E1M_X_I2C0 on E1M-X EVK, so the same
 * source scans whichever sensor bus the SoM exposes.  If the bus is
 * unavailable alp_i2c_open returns NULL; the example reports
 * alp_last_error and exits cleanly so a console harness can assert
 * it ran.
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
		/* Probe with a 1-byte read: a present chip ACKs its address
         * byte (we discard the data), an empty address NACKs.  We use a
         * read rather than a zero-length write because some controllers
         * — e.g. the DesignWare i2c_dw on Alif Ensemble — put nothing on
         * the bus for a zero-length transfer, so no device ever ACKs and
         * the scan finds nothing.  A 1-byte read is the portable probe. */
		uint8_t      scratch;
		alp_status_t s = alp_i2c_read(bus, addr, &scratch, 1);
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
