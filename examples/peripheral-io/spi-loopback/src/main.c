/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * spi-loopback — exercise alp_spi_open / alp_spi_transceive on
 * BOARD_SPI_ARDUINO.
 *
 * SPI is full-duplex: every byte you send out also clocks a byte
 * back from the slave.  alp_spi_transceive(tx, rx, len) does both
 * legs in one call.  This example sends four bytes and prints the
 * received values -- useful to verify wiring (loopback returns
 * what was sent), to read silicon IDs (slave returns its ID),
 * etc.
 *
 * Runs on both EVKs: BOARD_SPI_ARDUINO (from <alp/board.h>) resolves
 * to E1M_SPI1 on E1M EVK (through the on-module CC3501E) and
 * E1M_X_SPI1 on E1M-X EVK (level-shifted Arduino header SPI).
 */

#include <stdio.h>

#include "alp/peripheral.h"
#include "alp/board.h"

/* Sentinel meaning "no chip-select GPIO -- the controller manages
 * CS internally, or the device doesn't need one (e.g. shift
 * register chains).  Defined locally to keep the example
 * self-contained; real apps include this from <alp/peripheral.h>. */

int main(void)
{
	printf("[spi] open BOARD_SPI_ARDUINO @ 1 MHz mode 0\n");

	alp_spi_t *bus = alp_spi_open(&(alp_spi_config_t){
	    .bus_id = BOARD_SPI_ARDUINO,
	    /* 1 MHz is the conservative default; SPI tolerates up to
         * tens of MHz on most controllers.  Bump after confirming
         * the slave's max clock and that wires are short. */
	    .freq_hz = 1000000,
	    /* MODE_0 (CPOL=0, CPHA=0) is the most common configuration:
         * idle-low clock, sample on rising edge.  See the ALP SPI
         * mode chart in <alp/peripheral.h> for the other three
         * combinations. */
	    .mode = ALP_SPI_MODE_0,
	    /* 8 bits/word is universal; some SoCs support 16 or 32.  If
         * your slave needs a non-octet word width, this is the knob. */
	    .bits_per_word = 8,
	    /* No CS GPIO -- many SoCs handle CS automatically inside
         * the SPI controller.  When the slave needs a separate CS
         * pin (typical for multi-slave buses), set this to a
         * studio-resolved pin_id from <alp/e1m_pinout.h>. */
	    .cs_pin_id = ALP_SPI_NO_CS,
	});
	if (bus == NULL) {
		printf("[spi] open failed: alp_last_error=%d\n", (int)alp_last_error());
		printf("[spi] done\n");
		return 0;
	}

	/* Test pattern -- 0xAA, 0x55 alternates the data line every bit
     * (catches stuck-bit faults); 0x12 0x34 is non-symmetric and
     * shows endian handling.  rx[] receives whatever the slave
     * clocks back. */
	uint8_t      tx[4] = { 0xAA, 0x55, 0x12, 0x34 };
	uint8_t      rx[4] = { 0 };
	alp_status_t s     = alp_spi_transceive(bus, tx, rx, sizeof tx);
	printf("[spi] transceive -> status=%d  rx={%02x %02x %02x %02x}\n",
	       (int)s,
	       rx[0],
	       rx[1],
	       rx[2],
	       rx[3]);

	/* Close releases the bus handle.  CS line returns to its idle
     * state (high for active-low CS); clock and MOSI go to whatever
     * the controller's idle line state is. */
	alp_spi_close(bus);
	printf("[spi] done\n");
	return 0;
}
