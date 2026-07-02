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
 * On native_sim (CI lane) BOARD_SPI_ARDUINO (alp-spi1) is not
 * wired, so open() fails with ALP_ERR_NOT_READY; the example
 * prints the diagnostic and exits.  Either way the [spi-slave]
 * done marker latches the harness.
 *
 * Availability note: Zephyr's SPI slave support is itself patchy --
 * some SoC controller drivers reject SPI_OP_MODE_SLAVE.  See
 * <alp/peripheral.h> "SPI -- target (slave) mode" for the contract.
 *
 * Runs on both EVKs: BOARD_SPI_ARDUINO (from <alp/board.h>) resolves
 * to ALP_E1M_SPI1 on E1M EVK and ALP_E1M_X_SPI1 on E1M-X EVK.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "alp/peripheral.h"
#include "alp/board.h"
#include "alp/version.h"

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

/* Bounded wait per transfer.  A finite timeout is the SAFE pattern:
 * the thread gets control back even when the external controller
 * never clocks anything, so it can log, feed a watchdog, or shut
 * down cleanly (alp_spi_target_close refuses with ALP_ERR_BUSY while
 * a transceive is blocked -- a bounded wait is what lets you close).
 * UINT32_MAX would wait forever instead. */
#define TRANSFER_TIMEOUT_MS 5000u

/* Give up after this many timed-out waits in a row -- keeps the demo
 * bounded when nothing is wired to the bus. */
#define MAX_IDLE_WAITS 3u

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
		/* Report the SDK version -- <alp/version.h> gives every
		 * firmware image its version bytes for free, so a wire
		 * protocol never hardcodes them. */
		tx[1] = (uint8_t)ALP_VERSION_MAJOR;
		tx[2] = (uint8_t)ALP_VERSION_MINOR;
		tx[3] = (uint8_t)ALP_VERSION_PATCH;
		tx[4] = 'A'; /* app-defined tag char */
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
	 * but future backends (bridge links, vendor HAL bring-up) rely on
	 * it, and THEY can fail.  Check the return like any other call --
	 * an app that ignores it would run against a half-initialised
	 * SDK. */
	alp_status_t init_rc = alp_init();
	if (init_rc != ALP_OK) {
		printf("[spi-slave] alp_init failed: %d\n", (int)init_rc);
		printf("[spi-slave] done\n");
		return 1;
	}

	printf("[spi-slave] listening on BOARD_SPI_ARDUINO (mode 0, 8 bits)\n");

	alp_spi_target_t *tgt = alp_spi_target_open(&(alp_spi_target_config_t){
	    .bus_id        = BOARD_SPI_ARDUINO, /* /CS is board-routed and driven by
	                                         * the external controller. */
	    .mode          = ALP_SPI_MODE_0,    /* must match the controller */
	    .bits_per_word = 8,
	});
	if (tgt == NULL) {
		/* Common causes:
		 *   * ALP_ERR_NOT_READY -- alp-spi1 alias unset / device not
		 *     ready.  native_sim lands here: it wires only alp-spi0,
		 *     so BOARD_SPI_ARDUINO does not resolve.
		 *   * ALP_ERR_NOSUPPORT -- no slave mode in this backend (a
		 *     DRIVER-level gap surfaces on the first transceive
		 *     instead -- see <alp/peripheral.h>). */
		printf("[spi-slave] target open failed: alp_last_error=%d\n", (int)alp_last_error());
		printf("[spi-slave]   SPI target (slave) mode is unavailable here\n");
		printf("[spi-slave]   (native_sim does not wire BOARD_SPI_ARDUINO; on real\n");
		printf("[spi-slave]   hardware check the SoC driver supports slave mode)\n");
		printf("[spi-slave] done\n");
		return 0;
	}

	uint8_t tx[FRAME_LEN] = { 0 }; /* idle reply until the first command lands */
	uint8_t rx[FRAME_LEN] = { 0 };

	/* One timeout knob for the whole loop: dropped to UINT32_MAX
	 * (wait forever) when the controller driver has no async support
	 * -- bounded waits need it (Zephyr: CONFIG_SPI_ASYNC). */
	uint32_t timeout_ms = TRANSFER_TIMEOUT_MS;
	uint32_t served     = 0;
	uint32_t idle_waits = 0;

	while (served < TRANSFER_COUNT && idle_waits < MAX_IDLE_WAITS) {
		/* Stage the transfer and wait (bounded) for the external
		 * controller to clock it.  Zero rx first so a short transfer
		 * can't leave stale bytes from the previous frame in the
		 * un-clocked tail. */
		memset(rx, 0, sizeof(rx));
		size_t       got = 0;
		alp_status_t s   = alp_spi_target_transceive(tgt, tx, rx, FRAME_LEN, &got, timeout_ms);
		if (s == ALP_ERR_TIMEOUT || s == ALP_ERR_BUSY) {
			/* Nobody clocked us inside the window.  IMPORTANT: a
			 * timed-out transfer stays armed in the driver (SPI
			 * slaves have no portable cancel), so tx/rx must stay
			 * valid -- they are function-scope arrays reused by the
			 * next iteration, which satisfies that.  BUSY means the
			 * previous timed-out transfer is still pending. */
			idle_waits++;
			printf("[spi-slave] no transfer within %ums (%u/%u)\n",
			       (unsigned)timeout_ms,
			       (unsigned)idle_waits,
			       (unsigned)MAX_IDLE_WAITS);
			continue;
		}
		if (s == ALP_ERR_NOSUPPORT && timeout_ms != UINT32_MAX) {
			/* This driver can't do bounded waits (no async path).
			 * Degrade to the unbounded wait -- but say so, because
			 * the thread may now block indefinitely. */
			printf("[spi-slave] bounded wait unsupported; waiting forever\n");
			timeout_ms = UINT32_MAX;
			continue;
		}
		if (s != ALP_OK) {
			printf("[spi-slave] transfer %u: transceive -> %d\n", (unsigned)served, (int)s);
			break;
		}
		idle_waits = 0; /* traffic seen -- reset the give-up counter */
		printf("[spi-slave] transfer %u: rx=%u cmd=0x%02x\n",
		       (unsigned)served,
		       (unsigned)got,
		       (got > 0u) ? rx[0] : 0u);

		/* Decode what arrived and preload the reply the controller
		 * will clock out during the NEXT transfer. */
		build_reply(rx, got, tx);
		served++;
	}

	/* Close CAN refuse (ALP_ERR_BUSY) if a transfer were still blocked
	 * on another thread or armed after a timeout -- single-threaded
	 * and past the loop, a refusal here means the last timed-out
	 * transfer is still pending in the driver.  Report it instead of
	 * pretending the handle is gone. */
	alp_status_t close_rc = alp_spi_target_close(tgt);
	if (close_rc != ALP_OK) {
		printf("[spi-slave] close deferred: %d (transfer still armed)\n", (int)close_rc);
	}
	printf("[spi-slave] done\n");
	return 0;
}
