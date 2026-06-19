/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- SPI-slave transport wiring (the
 * DEFAULT host-control link).  Built ONLY for CC3501E_HAL_BACKEND=ti.
 *
 * Drives the SILICON-FREE byte seams in src/transport_spi.c from the
 * CC3501E's SPI peripheral via TI Drivers (SPI_open in SPI_PERIPHERAL +
 * SPI_MODE_CALLBACK).  Inter-chip pins (metadata/e1m_modules/aen/
 * inter-chip.tsv): SCLK/MOSI/MISO on CC3501E GPIO_27/28/29; Alif is
 * master, CC3501E is slave.
 *
 * ============== radio<->bridge coexistence (SHIP-CRITICAL) ============
 * The bridge SPI shares the CC35xx host-DMA controller with the on-chip
 * Wi-Fi/BLE radio.  The radio is a separate subsystem (NWP), but its HIF
 * bring-up runs on the AP core and re-inits the DMA block GLOBALLY: when
 * Wlan_Start (or a short Wlan_Get) runs, the bridge SPI's DMA transfer
 * stops completing and MISO goes silent -- bench-proven, and it happens
 * REGARDLESS of which DMA channel the bridge is pinned to (we tried ch11,
 * ch8/6, and the FREE ch12/13; all die the same way).  There is no
 * host-IRQ on this rev for the CC35 to signal busy/ready, so the bridge
 * simply cannot be serviced WHILE a radio op runs.
 *
 * The architecture that works WITH this constraint (submit -> radio-op
 * [bridge down] -> recover -> poll):
 *   1. Wlan_Start runs ONCE at boot, before any host traffic
 *      (cc3501e_hw_wifi_boot_start, main.c bringup_task).
 *   2. After EVERY radio op the worker calls bridge_transport_spi_hw_reinit()
 *      (worker.c / cc3501e_hw_ti.c) which does a REAL SPI_close + SPI_open +
 *      re-arm here, restoring the slave once the op is done.
 *   3. The host poll-retries on ALP_ERR_IO (link down during the op) with a
 *      budget > the op duration (chips/cc3501e/cc3501e.c poll_by_repeat).
 * The bridge DMA is pinned to the FREE channels 12/13 (cc3501e_aen{,_wifi}.
 * syscfg) -- harmless, and the right place if a future SDK stops the global
 * DMA re-init; but correctness here rests on the re-open, not the channel.
 *
 * The re-open MUST run from the bring-up/worker task, NOT the SPI callback:
 * a radio op leaves the slave's DMA dead, so the in-flight transfer never
 * completes and the callback never fires -- a flag the callback would have
 * to read is therefore useless.  reinit is only ever called right after a
 * radio op (when the slave is already dead), so SPI_close does not race a
 * live callback.
 *
 * ===================== 3-WIRE FRAMING (this rev) =====================
 * The current E1M-AEN rev wires ONLY SCLK/MOSI/MISO -- no CS, no host
 * IRQ/READY line (CS + IRQ are planned for the next board rev).  With no
 * CS edge to delimit transactions, framing is purely by FIXED CLOCK COUNT
 * in deterministic lockstep -- each side derives the next transfer's
 * length from a header it already exchanged:
 *
 *   1. master clocks 4    -> request header   (slave reads payload_len)
 *   2. master clocks N    -> request payload   (N = that payload_len)
 *   3. master clocks 4    -> reply header      (master reads reply len)
 *   4. master clocks M    -> reply payload      (M = that reply len)
 *
 * No CS means the CC3501E's SS pad must be tied to its asserted level on
 * the SoM so the slave is permanently selected.  No IRQ means the host
 * POLLS for the reply (it adds a settle gap then reads the reply header).
 * The completed request frame is replayed through the byte seams, so
 * framing/dispatch (and the host test) are identical to the stub path.
 *
 * CONFIG_SPI_0 is the SysConfig anchor for the inter-chip SPI instance,
 * resolved at bench-build time from the E1M-AEN board file.
 * =====================================================================
 */

#include <stddef.h>
#include <stdint.h>

#include "ti_drivers_config.h"

#include <ti/drivers/SPI.h>
#include <ti/drivers/dpl/ClockP.h> /* ClockP_usleep -- settle between SPI re-open retries */

#include "../../src/protocol.h"
#include "../../src/transport.h"

/* Deterministic lockstep phases (see file header). */
enum spi_phase {
	PH_REQ_HEADER = 0, /* clocking the 4-byte request header   */
	PH_REQ_PAYLOAD,    /* clocking payload_len request bytes   */
	PH_REPLY_HEADER,   /* clocking the 4-byte reply header     */
	PH_REPLY_PAYLOAD,  /* clocking the reply payload           */
};

static SPI_Handle     spi;
static enum spi_phase phase;

/* One in-flight request frame + its staged reply (header + max payload). */
static uint8_t  frame_buf[CC3501E_FRAME_MAX_BYTES];
static uint8_t  reply_buf[CC3501E_FRAME_MAX_BYTES];
static size_t   reply_len;
static uint16_t cur_payload_len;

/* Header-idle SYNC marker (ALP_CC3501E_SYNC_IDLE = 0xA5) driven on MISO while
 * the slave is parked at a frame boundary (clocking a request header).
 * CONTRACT-DEFINED, not bench diagnostics: with no CS on this rev the host keys
 * its byte-alignment sync + desync recovery off a run of 0xA5 (see
 * chips/cc3501e/cc3501e.c cc3501e_sync()).  During the request PAYLOAD phase the
 * slave drives NULL (0x00) instead -- 0xA5 marks ONLY the header-phase boundary,
 * so the host can distinguish "parked at a clean boundary" from "mid-payload". */
static uint8_t sync_idle[ALP_CC3501E_HEADER_BYTES];

/* P0-2 desync/probe re-arm counter (KEPT -- route through DIAG_GET_STATS for
 * link-health observability).  Counts header-phase re-arms triggered by a
 * reserved-range / all-0xFF header (a host sync-probe, or byte-misalignment). */
volatile uint32_t g_resync_count;

/* Count of bridge SPI (re-)opens: 1 = initial open, then +1 per radio-op
 * recovery (bridge_transport_spi_hw_reinit).  Observable for link-health. */
volatile uint32_t g_spi_reopen_count;

/* Arm a fixed-count slave transfer.  For RX, tx is the 0xA5 marker (header) or
 * NULL (payload -> 0x00 default fill on MISO); for TX, rx is NULL (the host's
 * MOSI dummies are discarded).  Non-blocking in SPI_MODE_CALLBACK: the DMA
 * (RX=ch12, TX=ch13) drains/fills the FIFO and the driver invokes on_transfer
 * when `count` frames have shifted. */
static void arm_transfer(void *rx, const void *tx, size_t count)
{
	static SPI_Transaction t; /* retained for the transfer's duration */
	t.count = count;
	t.txBuf = (void *)tx;
	t.rxBuf = rx;
	t.arg   = NULL;
	(void)SPI_transfer(spi, &t);
}

/* Re-arm the request-header phase driving the 0xA5 marker on MISO.  Used both
 * for the normal next-frame re-arm and the P0-2 desync/probe no-op re-arm. */
static void arm_request_header(void)
{
	phase = PH_REQ_HEADER;
	arm_transfer(frame_buf, sync_idle, ALP_CC3501E_HEADER_BYTES);
}

/* Replay the captured request frame through the silicon-free seams
 * (which build the staged reply), then drain that reply into reply_buf. */
static void dispatch_frame(size_t frame_len)
{
	spi_slave_cs_low();
	for (size_t i = 0; i < frame_len; i++) {
		spi_slave_rx_byte(frame_buf[i]);
	}
	spi_slave_cs_high();

	reply_len = 0u;
	while (spi_slave_tx_pending() && reply_len < sizeof(reply_buf)) {
		reply_buf[reply_len++] = spi_slave_tx_next_byte();
	}
	/* The reply is always header(4) + payload(>=1 status byte). */
}

/* SPI transfer-complete callback (driver SWI/HWI context).  Advances the
 * request-header -> request-payload -> reply-header -> reply-payload lockstep. */
static void on_transfer(SPI_Handle h, SPI_Transaction *t)
{
	(void)h;
	(void)t;

	switch (phase) {
	case PH_REQ_HEADER: {
		/* No-CS desync/probe guard (P0-2): a header whose cmd byte is in the
		 * reserved range (>= 0x80, which includes an all-0xFF idle/probe header)
		 * is NOT a valid v1 request.  It means the host is probing/re-syncing or
		 * byte alignment drifted -- do NOT dispatch; just re-arm the header phase
		 * (keep driving 0xA5) so the host's byte-walk lands on a clean boundary.
		 * Makes the sync handshake non-destructive. */
		if (frame_buf[0] >= ALP_CC3501E_CMD_RESERVED_VENDOR_BASE) {
			g_resync_count++;
			arm_request_header();
			break;
		}

		/* Bound the declared payload to the wire ceiling so a garbage length
		 * can't overrun the RX into frame_buf; an over-long declared length then
		 * fails the seam's captured-vs-declared check as RESP_ERR_PROTOCOL. */
		uint16_t plen = (uint16_t)frame_buf[2] | ((uint16_t)frame_buf[3] << 8);
		if (plen > ALP_CC3501E_MAX_PAYLOAD) {
			plen = ALP_CC3501E_MAX_PAYLOAD;
		}
		cur_payload_len = plen;
		if (plen == 0u) {
			dispatch_frame(ALP_CC3501E_HEADER_BYTES);
			phase = PH_REPLY_HEADER;
			arm_transfer(NULL, reply_buf, ALP_CC3501E_HEADER_BYTES);
		} else {
			phase = PH_REQ_PAYLOAD;
			/* NULL tx -> 0x00 on MISO during payload (0xA5 marks the header
			 * boundary only). */
			arm_transfer(&frame_buf[ALP_CC3501E_HEADER_BYTES], NULL, plen);
		}
		break;
	}
	case PH_REQ_PAYLOAD:
		dispatch_frame((size_t)ALP_CC3501E_HEADER_BYTES + cur_payload_len);
		phase = PH_REPLY_HEADER;
		arm_transfer(NULL, reply_buf, ALP_CC3501E_HEADER_BYTES);
		break;

	case PH_REPLY_HEADER:
		/* Reply header clocked out; now the reply payload (status + data
		 * = reply_len - 4 bytes, always >= 1). */
		phase = PH_REPLY_PAYLOAD;
		arm_transfer(
		    NULL, &reply_buf[ALP_CC3501E_HEADER_BYTES], reply_len - ALP_CC3501E_HEADER_BYTES);
		break;

	case PH_REPLY_PAYLOAD:
	default:
		/* Whole reply clocked -- clean frame boundary.  Re-arm the next request
		 * header driving 0xA5.  (A pending CMD_RESET is actioned by
		 * cc3501e_hw_tick() on the housekeeping task after this ack has gone out.) */
		arm_request_header();
		break;
	}
}

/* Open the bridge SPI0 slave (SPI_MODE_CALLBACK, DMA on the free ch12/13 per the
 * board file), fill the 0xA5 header-idle marker, and arm the first request
 * header.  Shared by init and reinit.  MUST run on the bring-up/worker task. */
static void spi_open_and_arm(void)
{
	for (size_t i = 0u; i < sizeof(sync_idle); i++) {
		sync_idle[i] = ALP_CC3501E_SYNC_IDLE;
	}

	SPI_Params params;
	SPI_Params_init(&params);
	params.mode                = SPI_PERIPHERAL;    /* CC35xx TI Drivers term for SPI slave */
	params.transferMode        = SPI_MODE_CALLBACK; /* DMA on the free ch12/13 (see file header) */
	params.transferCallbackFxn = on_transfer;
	params.frameFormat         = SPI_POL0_PHA0; /* mode 0, per the host driver / chip manifest */
	params.dataSize            = 8;

	/* Retry SPI_open: right after a psa_fwu flash burst (OTA FINISH) the shared
	 * DMA can be momentarily busy, so a single open intermittently returns NULL ->
	 * the slave stays dead and the host's poll times out (the OTA-finish flakiness,
	 * silicon 2026-06-19).  A few short-spaced retries make the re-arm reliable. */
	for (int attempt = 0; attempt < 8; attempt++) {
		spi = SPI_open(CONFIG_SPI_0, &params);
		if (spi != NULL) {
			break;
		}
		ClockP_usleep(2000); /* 2 ms settle */
	}
	g_spi_reopen_count++;
	if (spi == NULL) {
		/* No console this early; the host's PING simply never completes
		 * and bring-up code reports the dead link. */
		return;
	}

	g_resync_count = 0u;
	arm_request_header();
}

/* Re-open + re-arm the bridge slave after a radio op (boot Wlan_Start or a
 * worker Wlan_* body).  Called from the BRING-UP / worker task -- NEVER the SPI
 * callback -- and ONLY right after a radio op, when the radio's global DMA
 * re-init has already torn the slave's DMA down (the in-flight transfer no longer
 * completes, so no callback is pending to race the SPI_close).  A real
 * SPI_close + SPI_open is required: a flag the (now-dead) callback would have to
 * read can never be acted on.  After this the slave drives 0xA5 again and the
 * host's next poll lands cleanly. */
void bridge_transport_spi_hw_reinit(void)
{
	if (spi != NULL) {
		SPI_close(spi);
		spi = NULL;
	}
	spi_open_and_arm();
}

/* Quiesce the bridge slave + RELEASE its DMA (ch12/13) for the DURATION of a radio op
 * that re-arbitrates the shared HIF DMA (BLE-controller enable).  Unlike reinit (which
 * recovers AFTER an op), this runs BEFORE the op so the bridge SPI's DMA is not a live
 * second client contending with the HIF handshake the NWP must command-complete.  On
 * this rev there is NO host-driver mutex serialising bridge-HIF vs BLE-enable-HIF use
 * (ctrlCmdFw_LockHostDriver is a no-op), so the bridge MUST stand down explicitly.
 * SPI_transferCancel drops the armed CALLBACK transfer (frees its DMA) before SPI_close;
 * the worker calls bridge_transport_spi_hw_reinit() after the op to bring the slave back
 * (the host poll-retries on IO across the down-window). */
void bridge_transport_spi_hw_suspend(void)
{
	if (spi != NULL) {
		SPI_transferCancel(spi); /* cancel the in-flight lockstep transfer + its DMA */
		SPI_close(spi);
		spi = NULL;
	}
	phase = PH_REQ_HEADER;
}

void bridge_transport_spi_hw_init(void)
{
	spi_open_and_arm();
}
