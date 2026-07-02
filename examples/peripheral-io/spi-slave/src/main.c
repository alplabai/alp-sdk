/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * spi-slave -- claim the bus in target (slave) mode via
 * alp_spi_target_open and answer an external SPI controller.
 *
 * SPI slaves are transfer-based, not byte-callback based: the
 * external controller owns SCK + /CS, and because SPI is full-duplex
 * the slave cannot see a command byte and respond within the SAME
 * transfer.  The canonical protocol is therefore two-phase:
 *
 *   Transfer N   : controller sends [CMD][args...]; slave's TX is
 *                  whatever it preloaded (a status byte + the reply
 *                  to transfer N-1's command).
 *   Transfer N+1 : slave has decoded CMD and preloaded the reply.
 *
 * Example protocol (5-byte fixed frames):
 *   CMD_PING        0x01 -- reply echoes the ping payload
 *   CMD_GET_VERSION 0x02 -- reply carries [major minor patch tag]
 *   unknown              -- reply filled with 0xEE
 *
 * Test setup: wire SCK, MOSI, MISO, /CS, and GND to a second board
 * running examples/peripheral-io/spi-master (mode 0, 8 bits, and the
 * same 5-byte frame length).
 *
 * What success looks like (real hardware, controller clocking us):
 *
 *   [spi-slave] listening on BOARD_SPI_ARDUINO (mode 0, 8 bits)
 *   [spi-slave] transfer 0: rx=5 cmd=0x01
 *   [spi-slave] transfer 1: rx=5 cmd=0x02
 *   ...
 *   [spi-slave] done
 *
 * On native_sim (CI lane) there is no SPI slave emulation, so
 * open() fails with ALP_ERR_NOSUPPORT / ALP_ERR_NOT_READY; the
 * example prints the diagnostic and exits.  Either way the
 * [spi-slave] done marker latches the harness.
 *
 * Availability note: Zephyr's SPI slave support is itself patchy --
 * some SoC controller drivers reject SPI_OP_MODE_SLAVE.  See
 * <alp/peripheral.h> "SPI -- target (slave) mode" for the contract.
 *
 * Runs on both EVKs: BOARD_SPI_ARDUINO (from <alp/board.h>) resolves
 * to E1M_SPI1 on E1M EVK and E1M_X_SPI1 on E1M-X EVK.
 */

#include <stdio.h>
#include <string.h>

#include "alp/peripheral.h"
#include "alp/board.h"

/* Frame layout shared with the controller side: 1 command byte +
 * 4 payload bytes.  Fixed-length frames keep the slave's preload
 * logic trivial -- variable-length protocols put a length field in
 * the command byte instead. */
#define FRAME_LEN 5u

#define CMD_PING        0x01u /* echo payload back in the next frame */
#define CMD_GET_VERSION 0x02u /* reply with 4 version bytes */

/* How many controller transfers to serve before exiting.  Capped so
 * an operator gets a bounded demo; real firmware would loop forever. */
#define TRANSFER_COUNT 5u

/* Build the reply to a just-received frame into tx (FRAME_LEN bytes).
 * Byte 0 is a status placeholder (0x00 = OK) -- the controller
 * discards it; bytes 1..4 carry the previous command's response. */
static void build_reply(const uint8_t *rx_frame, size_t rx_len, uint8_t *tx)
{
	memset(tx, 0, FRAME_LEN);
	if (rx_len == 0u) {
		return; /* nothing received yet -- drive the idle reply */
	}
	switch (rx_frame[0]) {
	case CMD_PING:
		/* Echo the payload -- the simplest possible protocol;
		 * useful for connectivity probes. */
		memcpy(&tx[1], &rx_frame[1], FRAME_LEN - 1u);
		break;
	case CMD_GET_VERSION:
		tx[1] = 0x00u; /* major */
		tx[2] = 0x08u; /* minor (v0.8 today) */
		tx[3] = 0x00u; /* patch */
		tx[4] = 'A';   /* tag char */
		break;
	default:
		/* Unknown command -- fill with 0xEE so the controller can
		 * distinguish "bad command" from 0x00 padding. */
		memset(&tx[1], 0xEE, FRAME_LEN - 1u);
		break;
	}
}

int main(void)
{
	/* Bring up the SDK runtime before the first open() -- thin today,
	 * but future backends rely on it (see <alp/peripheral.h>). */
	(void)alp_init();

	printf("[spi-slave] listening on BOARD_SPI_ARDUINO (mode 0, 8 bits)\n");

	alp_spi_target_t *tgt = alp_spi_target_open(&(alp_spi_target_config_t){
	    .bus_id        = BOARD_SPI_ARDUINO, /* /CS is board-routed and driven by
	                                         * the external controller. */
	    .mode          = ALP_SPI_MODE_0,    /* must match the controller */
	    .bits_per_word = 8,
	});
	if (tgt == NULL) {
		/* Common causes:
		 *   * ALP_ERR_NOSUPPORT -- no slave mode in this backend or
		 *     controller driver.  Every native_sim build lands here
		 *     today: there is no SPI slave emulation.
		 *   * ALP_ERR_NOT_READY -- alp-spi1 alias unset / device
		 *     not ready on this board. */
		printf("[spi-slave] target open failed: alp_last_error=%d\n", (int)alp_last_error());
		printf("[spi-slave]   SPI target (slave) mode is unavailable on this build\n");
		printf("[spi-slave]   (native_sim has no slave-mode emulation; on real\n");
		printf("[spi-slave]   hardware check the SoC driver supports slave mode)\n");
		printf("[spi-slave] done\n");
		return 0;
	}

	uint8_t tx[FRAME_LEN] = { 0 }; /* idle reply until the first command lands */
	uint8_t rx[FRAME_LEN];

	for (uint32_t i = 0; i < TRANSFER_COUNT; i++) {
		/* Stage the transfer and block until the external controller
		 * clocks it (no timeout -- the controller decides when). */
		size_t       got = 0;
		alp_status_t s   = alp_spi_target_transceive(tgt, tx, rx, FRAME_LEN, &got);
		if (s != ALP_OK) {
			printf("[spi-slave] transfer %u: transceive -> %d\n", i, (int)s);
			break;
		}
		printf("[spi-slave] transfer %u: rx=%u cmd=0x%02x\n",
		       i,
		       (unsigned)got,
		       (got > 0u) ? rx[0] : 0u);

		/* Decode what arrived and preload the reply the controller
		 * will clock out during the NEXT transfer. */
		build_reply(rx, got, tx);
	}

	alp_spi_target_close(tgt);
	printf("[spi-slave] done\n");
	return 0;
}
