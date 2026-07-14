/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif-side host driver for the on-module TI CC3501E Wi-Fi 6 +
 * BLE 5.4 coprocessor.  See <alp/chips/cc3501e.h> for the public
 * lifecycle and <alp/protocol/cc3501e.h> for the wire protocol.
 *
 * Core module: lifecycle (init / reset / hard_reset / sync / get_version /
 * stream_write), the request/reply framing primitive (cc3501e_request), and
 * poll_by_repeat -- the retry wrapper every other cc3501e_<subsystem>.c
 * module builds its worker-routed helpers on (declared in
 * cc3501e_internal.h).
 *
 * Ships the call shape (init / reset / get_version / request /
 * set_event_callback / deinit) and the framing logic.  The actual
 * reset-pin pulse + WIFI.EN sequencing arrives once the EVK overlay
 * declares the Alif's P15_5 / P15_1_FLEX as GPIOs reachable via
 * alp_gpio_*; until then reset() returns NOSUPPORT cleanly.
 *
 * Wire framing matches the embedded firmware
 * (firmware/cc3501e/hal/ti/transport_hw_ti_spi.c): the current E1M-AEN
 * rev wires only SCLK/MOSI/MISO (no CS, no host IRQ -- both arrive next
 * rev), so a request/reply is clocked as four deterministic fixed-count
 * transfers in lockstep (request header, request payload, reply header,
 * reply payload) with a settle gap before the reply read.  The reply
 * payload's first byte is the response status (mapped via
 * resp_to_status); the data follows.
 */

#include <string.h>
#include <stdint.h>

#include "alp/peripheral.h"
#include "cc3501e_internal.h"

static void
encode_header(uint8_t *frame, alp_cc3501e_cmd_t cmd, uint8_t flags, uint16_t payload_len)
{
	frame[0] = (uint8_t)cmd;
	frame[1] = flags;
	frame[2] = (uint8_t)(payload_len & 0xFF);
	frame[3] = (uint8_t)((payload_len >> 8) & 0xFF);
}

static uint16_t decode_header_payload_len(const uint8_t *frame)
{
	return (uint16_t)frame[2] | ((uint16_t)frame[3] << 8);
}

alp_status_t cc3501e_init(cc3501e_t *ctx, alp_spi_t *bus)
{
	if (ctx == NULL || bus == NULL) return ALP_ERR_INVAL;
	memset(ctx, 0, sizeof(*ctx));
	ctx->bus         = bus;
	ctx->initialised = true;
	return ALP_OK;
}

alp_status_t cc3501e_reset(cc3501e_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (ctx->reset_pin == NULL || ctx->enable_pin == NULL) {
		/* The studio's pin allocator (or hand-written firmware
         * via alp_gpio_open) must populate enable_pin / reset_pin
         * before reset() is meaningful.  Until then there's no
         * line to pulse. */
		return ALP_ERR_NOSUPPORT;
	}
	/* Reset sequence per TI SWRU626 §7.1.5 (CC3501E technical
     * reference manual):
     *
     *   1. Assert nRESET low while bringing rails down so the
     *      chip stays clamped through the supply transition.
     *   2. Drop WIFI_EN low; wait briefly for the rails to
     *      discharge (10us is comfortably above the rail RC).
     *   3. Raise WIFI_EN; wait ~5 ms for the supply ramps to
     *      stabilise (typical PMIC soft-start window).
     *   4. Hold nRESET low for >= 10 us per §7.1.5 after the
     *      supplies are valid.
     *   5. Release nRESET; wait the T1+T2+T3+T4 boot budget
     *      (~900 ms typical for BL1 + BL2 + Chain-of-Trust)
     *      before the first PING is meaningful.
     *
     * Total blocking time: ~905 ms.  Callers that don't want
     * the synchronous wait can call cc3501e_reset asynchronously
     * (kicked off from a worker thread) and poll via PING; v0.3.x
     * adds a non-blocking variant once the firmware's "boot done"
     * GPIO is wired. */
	(void)alp_gpio_write(ctx->reset_pin, false);
	(void)alp_gpio_write(ctx->enable_pin, false);
	/* COLD-BOOT POWER SEQUENCE (2026-06-17): generous, cold-safe timings.
	 * The CC3501E's secure boot (BL1->BL2->vendor image) runs ONLY on a true
	 * cold power-on; on this E1M-AEN board VPA(3.3V) is gated by WIFI_EN via the
	 * U1 load switch and the HFXT(52 MHz) crystal must be stable before/through
	 * the SES launch.  The earlier 10us discharge / 5ms ramp were too aggressive
	 * for a clean cold POR (warm reset hid it), so widen every window:
	 *   - 50 ms discharge so the rails fully collapse => the CC35 sees a real POR
	 *     (not a brown-out that skips Chain-of-Trust re-init), and
	 *   - 100 ms after WIFI_EN so VPA + the crystal are fully settled before
	 *     nRESET is released (TI SWRU626 §2.2.2.1: all supplies valid before
	 *     nRESET), with a 1 ms asserted-low hold, and
	 *   - 1500 ms boot budget before the first PING. */
	alp_delay_ms(50u);
	(void)alp_gpio_write(ctx->enable_pin, true);
	alp_delay_ms(100u);
	/* nRESET stays low through the rail ramp; this assignment is
     * idempotent but kept explicit for clarity. */
	(void)alp_gpio_write(ctx->reset_pin, false);
	alp_delay_ms(1u);
	(void)alp_gpio_write(ctx->reset_pin, true);
	alp_delay_ms(1500u);
	/* Puya-flash (PY25Q64LB / 64Mbit) cold-boot workaround -- TI SDK bug confirmed
	 * by the CC35 module vendor 2026-06-18: the FIRST boot after a cold power-on
	 * mis-reads the Puya flash (the bug is specific to 32/64Mbit Puya parts), so the
	 * secure boot never launches the vendor image (host sees reqhdr_rx=0xFFFFFFFF).
	 * Re-boot once with the rails kept up; the second boot reads the now-settled
	 * flash and launches normally.  Validated on silicon (cold reqhdr_rx
	 * 0xFFFFFFFF -> 0x5A5A5A5A, ping_ok climbing, after one hard reset).  The
	 * bringup soak calls cc3501e_hard_reset() again if a single re-boot is not
	 * enough.  Remove once TI ships the Puya flash fix. */
	return cc3501e_hard_reset(ctx);
}

alp_status_t cc3501e_hard_reset(cc3501e_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (ctx->reset_pin == NULL) return ALP_ERR_NOSUPPORT;
	/* Pulse nRESET while keeping WIFI_EN asserted so the module re-boots WITHOUT a
	 * cold power cycle (a cold cycle would re-trigger the Puya-flash bug).  This is
	 * the "second boot" of the cold-boot workaround and the retry primitive the
	 * bringup soak uses when a cold-booted module has not come up yet. */
	(void)alp_gpio_write(ctx->reset_pin, false); /* assert nRESET; rails stay up */
	alp_delay_ms(50u);
	(void)alp_gpio_write(ctx->reset_pin, true); /* release -> re-boot */

	/* BLIND boot settle -- NO clocking until the slave is armed.  This is the
	 * cold first-contact fix: on the CS-less fixed-count link, any byte the host
	 * clocks BEFORE the slave's SPI is armed (e.g. a readiness poll's probes) is
	 * not consumed by the slave, which sets a permanent 1-byte frame offset that
	 * cannot self-correct (clocking advances both sides equally).  So the host
	 * stays QUIET while the module boots + arms; the slave then parks driving the
	 * 0xA5 marker, and the caller's first PING lands at the slave's fresh frame
	 * boundary = aligned.  The Wi-Fi build cold-boots (Puya double-boot + crypto
	 * Board_init) in ~2-3 s; 3.5 s covers it with margin and no clocking. */
	alp_delay_ms(3500u);
	return ALP_OK;
}

/* Map a CC3501E response status byte (first reply-payload byte, per
 * <alp/protocol/cc3501e.h>) onto the SDK's alp_status_t. */
static alp_status_t resp_to_status(uint8_t resp)
{
	switch (resp) {
	case ALP_CC3501E_RESP_OK:
		return ALP_OK;
	case ALP_CC3501E_RESP_ERR_INVALID:
		return ALP_ERR_INVAL;
	case ALP_CC3501E_RESP_ERR_BUSY:
		return ALP_ERR_BUSY;
	case ALP_CC3501E_RESP_ERR_TIMEOUT:
		return ALP_ERR_TIMEOUT;
	case ALP_CC3501E_RESP_ERR_NO_MEM:
		return ALP_ERR_NOMEM;
	case ALP_CC3501E_RESP_ERR_NOT_READY:
		return ALP_ERR_NOT_READY;
	case ALP_CC3501E_RESP_ERR_VERSION:
		return ALP_ERR_VERSION;
	case ALP_CC3501E_RESP_ERR_RADIO:
	case ALP_CC3501E_RESP_ERR_PROTOCOL:
	case ALP_CC3501E_RESP_ERR_INTERNAL:
	default:
		return ALP_ERR_IO;
	}
}

alp_status_t cc3501e_sync(cc3501e_t *ctx, uint32_t timeout_ms)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;

	/* MOSI is don't-care while syncing; 0xFF reads as a reserved-range
	 * ("no-op probe") header on the slave, which re-arms its header phase
	 * (firmware P0-2) so it keeps driving the 0xA5 marker -- making this walk
	 * non-destructive. */
	uint8_t tx = 0xFFu;
	uint8_t rx = 0u;

	/* Worst case, clock through one full in-flight request+reply frame to
	 * reach the slave's parked header boundary; "parked" = a run of two
	 * header-widths of 0xA5 (rejects a stray 0xA5 byte inside reply data). */
	const uint32_t walk_max = 2u * (uint32_t)(ALP_CC3501E_HEADER_BYTES + ALP_CC3501E_MAX_PAYLOAD);
	const uint32_t run_need = 2u * (uint32_t)ALP_CC3501E_HEADER_BYTES;
	const uint32_t attempts = (timeout_ms > 0u) ? timeout_ms : 1u;

	for (uint32_t a = 0u; a < attempts; a++) {
		uint32_t run = 0u;
		for (uint32_t w = 0u; w < walk_max; w++) {
			if (alp_spi_transceive(ctx->bus, &tx, &rx, 1u) != ALP_OK) return ALP_ERR_IO;
			if (rx == ALP_CC3501E_SYNC_IDLE) {
				if (++run >= run_need) return ALP_OK; /* aligned at a clean header boundary */
			} else {
				run = 0u;
			}
		}
		alp_delay_ms(1u); /* let the slave drain any in-flight frame + re-arm header phase */
	}
	return ALP_ERR_TIMEOUT;
}

/* Inter-phase settle (CS-less lockstep): time given to the CC3501E SPI-slave ISR
 * to arm the next fixed-count transfer (request payload, reply payload) before
 * the host clocks it.  ~µs is enough; 200 µs is comfortably safe and negligible
 * vs the per-request budget.  The r2 bridge (CS + host-IRQ) removes the need. */
#define CC3501E_PHASE_SETTLE_US 200u

/* READY gate for the r2 SS0 + host-IRQ bridge.  When ctx->ready_pin is
 * populated (the CC35 GPIO17 -> Alif P2_6 line is wired + opened as an input),
 * wait for it HIGH -- the slave drives it HIGH when its SPI slave is armed+idle
 * -- before clocking a reply phase, instead of a fixed settle gap.  This tracks
 * the slave's actual re-arm rather than guessing, so slow (Wi-Fi/BLE) replies
 * no longer need a conservative fixed delay.  Opt-in + degrades safely: a NULL
 * ready_pin (CS-less r1 boards) or a line that never asserts falls back to the
 * fixed gap.  See project_cc3501e_link_topology. */
static void cc3501e_reply_gate(const cc3501e_t *ctx, uint32_t fallback_us)
{
	if (ctx->ready_pin != NULL) {
		/* Opportunistic: a bounded burst of cheap polls catches an already-armed
		 * slave (fast READY assert) and short-cuts the wait.  If the line isn't
		 * asserted -- a slow op, or an IRQ bodge not yet HW-validated (P2_6 reads
		 * 0 on the current bench) -- fall through to the proven fixed gap.  So the
		 * gate never stalls and never costs more than a short burst + the gap. */
		bool level = false;
		for (uint32_t i = 0; i < 64u; ++i) {
			if (alp_gpio_read(ctx->ready_pin, &level) == ALP_OK && level) {
				return;
			}
		}
	}
	alp_delay_us(fallback_us);
}

alp_status_t cc3501e_request(cc3501e_t        *ctx,
                             alp_cc3501e_cmd_t cmd,
                             const uint8_t    *tx_payload,
                             size_t            tx_len,
                             uint8_t          *rx_buf,
                             size_t            rx_cap,
                             size_t           *rx_len,
                             uint32_t          timeout_ms)
{
	(void)timeout_ms; /* Reserved for a future IRQ-driven wait (next HW rev). */
	if (rx_len != NULL) *rx_len = 0;
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (tx_len > ALP_CC3501E_MAX_PAYLOAD) return ALP_ERR_INVAL;
	if (tx_payload == NULL && tx_len > 0) return ALP_ERR_INVAL;

	/*
     * 3-wire deterministic framing (this HW rev wires only SCLK/MOSI/MISO
     * -- no CS, no host IRQ; CS + IRQ arrive next rev).  Each transfer's
     * length is derived from a header already exchanged, so master + slave
     * stay in lockstep without a CS edge.  Matches the firmware SPI-slave
     * state machine in firmware/cc3501e/hal/ti/transport_hw_ti_spi.c.
     *
     *   1. send request header (4)        3. read reply header (4)
     *   2. send request payload (tx_len)  4. read reply payload (status+data)
     */
	/* Gate READY before the REQUEST header too.  After the slave sends a reply it
	 * re-arms its header phase in its ISR; a spaced request (soak loop, bring-up)
	 * has ample idle time so the header always landed on an armed slave.  But a
	 * TIGHT back-to-back loop -- streaming via cc3501e_stream_write -- clocks the
	 * next header the instant the prior reply is read, racing that re-arm: the
	 * first frame acks, then every following frame desyncs (bench 2026-07-04:
	 * dma_stream_iters stuck at 1).  READY tracks the actual header-arm; on a
	 * CS-less r1 board with no ready_pin the fallback is the same short settle the
	 * other phases use. */
	cc3501e_reply_gate(ctx, CC3501E_PHASE_SETTLE_US);
	encode_header(ctx->tx_scratch, cmd, ALP_CC3501E_FLAG_RESP_REQUIRED, (uint16_t)tx_len);
	alp_status_t s =
	    alp_spi_transceive(ctx->bus, ctx->tx_scratch, ctx->rx_scratch, ALP_CC3501E_HEADER_BYTES);
	if (s != ALP_OK) return s;
	if (tx_len > 0) {
		/* Inter-phase settle (CS-less lockstep): the slave arms the request-PAYLOAD
		 * transfer in its SPI ISR only AFTER the header transfer completes.  Clocking
		 * the payload back-to-back (no gap) races that re-arm -> the payload bytes are
		 * dropped + the frame desyncs.  Header-only requests (PING / the argless worker
		 * ops) have no payload phase so they were fine; payload requests (OTA_WRITE,
		 * CONNECT, GPIO_WRITE) need this gap (root-caused on silicon 2026-06-19, where
		 * OTA streaming timed out per-chunk without it). */
		alp_delay_us(CC3501E_PHASE_SETTLE_US);
		s = alp_spi_transceive(ctx->bus, tx_payload, ctx->rx_scratch, tx_len);
		if (s != ALP_OK) return s;
	}

	/* Wait for the slave to dispatch + arm its reply before we read: the
	 * READY gate tracks it via the host-IRQ line when wired, else a fixed gap. */
	cc3501e_reply_gate(ctx, 200u);

	/* Dummies for the read transactions (MOSI is don't-care on a read). */
	memset(ctx->tx_scratch, 0xFF, sizeof(ctx->tx_scratch));

	/* 3. Reply header -> learn the reply payload length. */
	s = alp_spi_transceive(ctx->bus, ctx->tx_scratch, ctx->rx_scratch, ALP_CC3501E_HEADER_BYTES);
	if (s != ALP_OK) return s;
	uint16_t resp_payload_len = decode_header_payload_len(ctx->rx_scratch);
	/* Desync detection (no CS to recover on): a valid reply header ECHOES the
     * request opcode (protocol_build_reply sets reply[0]=cmd) and declares a
     * payload in [1..MAX].  An all-0xA5 header means the slave is parked at a
     * frame boundary (we were misaligned); any other mismatch is lockstep
     * drift.  Either way, re-establish byte alignment so the NEXT request lands
     * clean, and report IO so the caller retries (the soak/bring-up loops do). */
	const bool hdr_ok = (ctx->rx_scratch[0] == (uint8_t)cmd) && (resp_payload_len >= 1u) &&
	                    (resp_payload_len <= ALP_CC3501E_MAX_PAYLOAD);
	if (!hdr_ok) {
		/* Do NOT byte-walk cc3501e_sync() here: on the 4-byte fixed-count lockstep
		 * the 1-byte walk PARKS the slave (proven on silicon -- reply_hdr stuck at
		 * 0xA5A5A5A5, link never recovers).  Return IO so the caller re-issues a
		 * clean 4-byte transaction instead (re-aligns when the slave is aligned). */
		return ALP_ERR_IO;
	}

	/* Same READY gate before the reply PAYLOAD phase (the slave re-arms it in
	 * its ISR only after the reply-header transfer completes). */
	cc3501e_reply_gate(ctx, CC3501E_PHASE_SETTLE_US);
	/* 4. Reply payload: status byte followed by the response data. */
	s = alp_spi_transceive(ctx->bus, ctx->tx_scratch, ctx->rx_scratch, resp_payload_len);
	if (s != ALP_OK) return s;

	const uint8_t resp     = ctx->rx_scratch[0];
	const size_t  data_len = (size_t)resp_payload_len - 1u;
	if (data_len > 0u && rx_buf != NULL) {
		const size_t n = (data_len > rx_cap) ? rx_cap : data_len;
		memcpy(rx_buf, &ctx->rx_scratch[1], n);
		if (rx_len != NULL) *rx_len = n;
	}
	return resp_to_status(resp);
}

alp_status_t cc3501e_get_version(cc3501e_t *ctx, uint16_t *version_out)
{
	if (version_out == NULL) return ALP_ERR_INVAL;
	uint8_t      reply[2] = { 0 };
	size_t       got      = 0;
	alp_status_t s =
	    cc3501e_request(ctx, ALP_CC3501E_CMD_GET_VERSION, NULL, 0, reply, sizeof(reply), &got, 100);
	if (s != ALP_OK) return s;
	if (got < sizeof(reply)) return ALP_ERR_IO;
	*version_out = (uint16_t)reply[0] | ((uint16_t)reply[1] << 8);
	return ALP_OK;
}

alp_status_t cc3501e_stream_write(cc3501e_t *ctx, const uint8_t *data, size_t len)
{
	if (ctx == NULL || (data == NULL && len > 0u)) return ALP_ERR_INVAL;
	if (len > (size_t)(ALP_CC3501E_MAX_PAYLOAD - ALP_CC3501E_HEADER_BYTES)) {
		return ALP_ERR_INVAL;
	}
	/* One framed bulk frame: the request PAYLOAD phase clocks @len bytes in a
	 * single transfer, which takes the host DMA path when @len >= the SPI DMA
	 * threshold (CONFIG_SPI_DW_ALIF_DMA_MIN_LEN).  The firmware sinks + acks it,
	 * so the link stays framed -- send these back-to-back for a bulk stream. */
	return cc3501e_request(ctx, ALP_CC3501E_CMD_STREAM_WRITE, data, len, NULL, 0u, NULL, 200u);
}

/* Poll-by-repeat backoff: how long to wait between BUSY repeats. */
#define CC3501E_POLL_GAP_MS 50u

alp_status_t poll_by_repeat(cc3501e_t        *ctx,
                            alp_cc3501e_cmd_t cmd,
                            const uint8_t    *tx_payload,
                            size_t            tx_len,
                            uint8_t          *rx_buf,
                            size_t            rx_cap,
                            size_t           *rx_len,
                            uint32_t          timeout_ms)
{
	/* Budget is coarse-grained in CC3501E_POLL_GAP_MS slices; always make at
	 * least one attempt even with a zero timeout. */
	uint32_t     remaining = (timeout_ms > 0u) ? timeout_ms : 1u;
	alp_status_t s;
	for (;;) {
		s = cc3501e_request(
		    ctx, cmd, tx_payload, tx_len, rx_buf, rx_cap, rx_len, CC3501E_REQ_TMO_MS);
		if (s != ALP_ERR_BUSY && s != ALP_ERR_IO) {
			return s; /* OK or a non-retryable error -- done. */
		}
		if (remaining == 0u) {
			return ALP_ERR_TIMEOUT;
		}
		uint32_t gap = (remaining < CC3501E_POLL_GAP_MS) ? remaining : CC3501E_POLL_GAP_MS;
		alp_delay_ms(gap);
		remaining -= gap;
	}
}
