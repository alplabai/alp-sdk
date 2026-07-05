/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif-side host driver for the on-module TI CC3501E Wi-Fi 6 +
 * BLE 5.4 coprocessor.  See <alp/chips/cc3501e.h> for the public
 * lifecycle and <alp/protocol/cc3501e.h> for the wire protocol.
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

#include "alp/chips/cc3501e.h"
#include "alp/peripheral.h"

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

/* Cold-boot self-recovery soak (the CUSTOMER cold-boot solution): the CC3501E's
 * first boot after a cold power-on can mis-read its Puya flash (reqhdr_rx=
 * 0xFFFFFFFF, vendor image never launches, SPI slave never arms). The fix is a
 * WARM re-boot (nRESET pulse, rails held) that reads the now-settled flash -- but
 * ONE re-boot is not always enough, so cc3501e_reset RETRIES the warm re-boot,
 * probing cc3501e_sync between each, until the slave arms. This runs unattended
 * inside cc3501e_reset/init, so a plain application self-recovers on cold
 * power-on WITHOUT any external warm reset (which a fielded product cannot rely
 * on). Bounded so a genuinely dead module still returns an error. */
#define CC3501E_COLD_BOOT_MAX_REBOOTS 8u    /* warm re-boots after the first before giving up */
#define CC3501E_COLD_BOOT_SYNC_MS     1500u /* per-probe sync budget (slave-arm window) */

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
	 * enough, so the SELF-RECOVERY SOAK below re-boots (warm) + re-probes until
	 * the slave arms -- unattended, so a cold-booting app recovers on its own.
	 * Remove the soak once TI ships the Puya flash fix. */
	(void)cc3501e_hard_reset(ctx); /* first warm re-boot (reads settled flash) */
	for (unsigned int i = 0u; i < CC3501E_COLD_BOOT_MAX_REBOOTS; i++) {
		/* cc3501e_sync clocks a non-destructive 0xFF probe looking for the
		 * slave's parked 0xA5 armed-marker; ALP_OK => the vendor image launched
		 * and the SPI slave is framing => the Puya flash settled. */
		if (cc3501e_sync(ctx, CC3501E_COLD_BOOT_SYNC_MS) == ALP_OK) {
			return ALP_OK;
		}
		(void)cc3501e_hard_reset(ctx); /* still 0xFFFFFFFF -- warm re-boot, rails held */
	}
	/* Final probe: OK if it armed on the last re-boot, else surface the failure
	 * (a genuinely dead / unpowered / unbodged module). */
	return cc3501e_sync(ctx, CC3501E_COLD_BOOT_SYNC_MS);
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

/* ------------------------------------------------------------------ */
/* Wi-Fi host helpers                                                  */
/*                                                                     */
/* Each is a thin wrapper over cc3501e_request matching the opcodes +  */
/* payloads in <alp/protocol/cc3501e.h>.  WIRE GAPS (the protocol      */
/* header, owned by the firmware-side agent, has opcodes but NO reply  */
/* payload structs for these -- noted per-helper):                     */
/*   - GET_MAC (0x03): reply data assumed to be the 6 MAC bytes.       */
/*   - WIFI_GET_RSSI (0x16): reply data assumed to be one int8 dBm.    */
/*   - WIFI_GET_IP (0x17): reply data assumed to be 4 IPv4 octets.     */
/*   - WIFI_SCAN_START (0x10): the header defines alp_cc3501e_scan_     */
/*     result_t and documents scan results as ASYNC events             */
/*     (EVT_WIFI_SCAN_RESULT 0x18) -- there is no synchronous          */
/*     count/list envelope.  This rev has no async-event line, so the  */
/*     helper assumes the firmware returns the packed records as the   */
/*     SCAN_START reply payload (each fixed 10-byte header + inline     */
/*     ssid_len SSID bytes).                                           */
/* ------------------------------------------------------------------ */

/* Poll-by-repeat backoff: how long to wait between BUSY repeats and the
 * per-request reply timeout passed down to cc3501e_request. */
#define CC3501E_POLL_GAP_MS 50u
#define CC3501E_REQ_TMO_MS  100u

/* Minimum budget for an op that can overlap a radio bring-up.  On this
 * no-host-IRQ rev the bridge link is DOWN while the CC35 runs a radio op
 * (Wlan_Start/RoleUp can take SECONDS); requests during that window read back
 * IO and must keep retrying.  Floor the get-MAC poll budget here so a small
 * caller timeout can't give up inside the down-window before the radio is up. */
#define CC3501E_WIFI_DOWN_WINDOW_MS 10000u

/* BLE enable stands the bridge down for the NWP BLE-controller cold-init (a HIF
 * control-cmd round-trip, ~10-15 s) + the NimBLE host sync; floor the host poll well
 * above that so a slow-but-working enable is not misread as a timeout. */
#define CC3501E_BLE_ENABLE_WINDOW_MS 90000u

/* Re-issue one request while the firmware is unavailable, until it resolves
 * (OK / hard error) or the budget elapses.  Two retryable conditions:
 *
 *   - ALP_ERR_BUSY : the firmware worker is still running the op (poll-by-repeat
 *     -- the host re-issues to collect the cached result once it lands).
 *   - ALP_ERR_IO   : the bridge link was DOWN for this transaction.  On this
 *     no-host-IRQ rev the CC35 cannot service the inter-chip SPI slave WHILE it
 *     runs a radio op (Wlan_Start at boot, or the worker's Wlan_* body), so a
 *     request that overlaps the op reads back desynced (cc3501e_request returns
 *     IO at the reply-header sanity check).  The firmware re-syncs the slave at
 *     a clean boundary right after the op, so a retry lands cleanly -- treat IO
 *     as transient here and keep polling for the whole budget.
 *
 * Returns the final cc3501e_request status; ALP_ERR_TIMEOUT if it never
 * resolved within the budget.  The caller's budget must therefore cover the
 * longest down-window (Wlan_Start/op, seconds) -- see cc3501e_wifi_get_mac. */
static alp_status_t poll_by_repeat(cc3501e_t        *ctx,
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

alp_status_t cc3501e_wifi_get_mac(cc3501e_t *ctx, uint8_t mac[CC3501E_MAC_LEN], uint32_t timeout_ms)
{
	if (mac == NULL) return ALP_ERR_INVAL;

	/* Floor the budget to cover the radio down-window: a GET_MAC issued while
	 * the CC35 is still bringing the radio up (boot Wlan_Start) sees the bridge
	 * down (IO) and must keep retrying until the radio is up + the slave
	 * re-syncs.  A caller passing a short timeout would otherwise give up mid
	 * down-window and report a spurious failure. */
	uint32_t budget = timeout_ms;
	if (budget < CC3501E_WIFI_DOWN_WINDOW_MS) budget = CC3501E_WIFI_DOWN_WINDOW_MS;

	uint8_t      reply[CC3501E_MAC_LEN] = { 0 };
	size_t       got                    = 0;
	alp_status_t s =
	    poll_by_repeat(ctx, ALP_CC3501E_CMD_GET_MAC, NULL, 0, reply, sizeof(reply), &got, budget);
	if (s != ALP_OK) return s;
	if (got < CC3501E_MAC_LEN) return ALP_ERR_IO; /* short reply -- firmware/wire gap */
	memcpy(mac, reply, CC3501E_MAC_LEN);
	return ALP_OK;
}

/* On-wire fixed header of a scan record (alp_cc3501e_scan_result_t without the
 * inline SSID): bssid[6] + rssi(1) + channel(1) + security(1) + ssid_len(1). */
#define CC3501E_SCAN_REC_HDR 11u

/* Decode the raw TI 16-bit SecurityInfo. The sec-type bitmap lives in the high
 * byte ((info >> 8) & 0x3f = WLAN_SCAN_RESULT_SEC_TYPE_BITMAP): SAE bits
 * (0x08|0x10) mark WPA3, 0x04 = WPA2, 0x02 = WPA, 0x01 = WEP, 0 = open. */
cc3501e_wifi_sec_t cc3501e_wifi_sec_kind(uint16_t security_info)
{
	uint8_t sec = (uint8_t)((security_info >> 8) & 0x3fu);
	if (sec == 0u) return CC3501E_WIFI_SEC_OPEN;
	if (sec & (0x08u | 0x10u)) return CC3501E_WIFI_SEC_WPA3;
	if (sec & 0x04u) return CC3501E_WIFI_SEC_WPA2;
	if (sec & 0x02u) return CC3501E_WIFI_SEC_WPA;
	if (sec & 0x01u) return CC3501E_WIFI_SEC_WEP;
	return CC3501E_WIFI_SEC_UNKNOWN;
}

const char *cc3501e_wifi_sec_name(uint16_t security_info)
{
	switch (cc3501e_wifi_sec_kind(security_info)) {
	case CC3501E_WIFI_SEC_OPEN:
		return "open";
	case CC3501E_WIFI_SEC_WEP:
		return "wep";
	case CC3501E_WIFI_SEC_WPA:
		return "wpa";
	case CC3501E_WIFI_SEC_WPA2:
		return "wpa2";
	case CC3501E_WIFI_SEC_WPA3:
		return "wpa3";
	default:
		return "sec?";
	}
}

alp_status_t cc3501e_wifi_scan(cc3501e_t             *ctx,
                               cc3501e_scan_record_t *out_records,
                               size_t                 cap,
                               size_t                *count,
                               uint32_t               timeout_ms)
{
	if (out_records == NULL && cap > 0u) return ALP_ERR_INVAL;
	if (count != NULL) *count = 0;

	/* The scan records can fill the reply payload; receive into the driver's
	 * own scratch-sized buffer (a local mirror keeps cc3501e_request's scratch
	 * free for the framing). */
	static uint8_t scan_buf[ALP_CC3501E_MAX_PAYLOAD];
	size_t         got = 0;
	alp_status_t   s   = poll_by_repeat(ctx,
	                                    ALP_CC3501E_CMD_WIFI_SCAN_START,
	                                    NULL,
	                                    0,
	                                    scan_buf,
	                                    sizeof(scan_buf),
	                                    &got,
	                                    timeout_ms);
	if (s != ALP_OK) return s;

	/* Walk the packed records: each is a 10-byte fixed header immediately
	 * followed by ssid_len inline SSID bytes (no padding). */
	size_t off = 0;
	size_t n   = 0;
	while (off + CC3501E_SCAN_REC_HDR <= got && n < cap) {
		const uint8_t *rec      = &scan_buf[off];
		uint8_t        ssid_len = rec[10];
		if (off + CC3501E_SCAN_REC_HDR + (size_t)ssid_len > got) {
			break; /* truncated trailing record -- stop cleanly */
		}
		cc3501e_scan_record_t *out = &out_records[n];
		memcpy(out->bssid, &rec[0], 6);
		out->rssi_dbm = (int8_t)rec[6];
		out->channel  = rec[7];
		/* Raw 16-bit SecurityInfo, little-endian (firmware packs both bytes at
		 * rec[8..9]; the sec-type lives in the high byte). */
		out->security_info = (uint16_t)rec[8] | ((uint16_t)rec[9] << 8);
		out->ssid_len      = ssid_len;
		uint8_t copy       = (ssid_len > CC3501E_SSID_MAX) ? (uint8_t)CC3501E_SSID_MAX : ssid_len;
		memcpy(out->ssid, &rec[CC3501E_SCAN_REC_HDR], copy);
		out->ssid[copy] = '\0';
		off += CC3501E_SCAN_REC_HDR + (size_t)ssid_len;
		n++;
	}
	if (count != NULL) *count = n;
	return ALP_OK;
}

alp_status_t cc3501e_wifi_scan_stop(cc3501e_t *ctx)
{
	/* Abort an in-progress scan (WIFI_SCAN_STOP, 0x11).  No payload, no reply
	 * data -- success is the OK status.  The firmware tears the scan down as a
	 * radio op, so the bridge can be briefly down (IO/BUSY) mid-abort; floor the
	 * budget to the radio down-window and let poll_by_repeat retry, exactly like
	 * cc3501e_wifi_get_mac. */
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_WIFI_SCAN_STOP, NULL, 0, NULL, 0, NULL, CC3501E_WIFI_DOWN_WINDOW_MS);
}

alp_status_t cc3501e_wifi_connect(
    cc3501e_t *ctx, const char *ssid, uint8_t sec_type, const char *pass, uint32_t timeout_ms)
{
	if (ssid == NULL) return ALP_ERR_INVAL;
	size_t ssid_len = strlen(ssid);
	size_t psk_len  = (pass != NULL) ? strlen(pass) : 0u;
	if (ssid_len > 32u || psk_len > 64u) return ALP_ERR_INVAL;

	/* On-wire payload: alp_cc3501e_wifi_connect_t header (4 B) + inline SSID +
	 * inline passphrase, all packed with no padding. */
	uint8_t                    payload[sizeof(alp_cc3501e_wifi_connect_t) + 32u + 64u];
	alp_cc3501e_wifi_connect_t hdr = {
		.ssid_len = (uint8_t)ssid_len,
		.psk_len  = (uint8_t)psk_len,
		.security = sec_type,
		.reserved = 0u,
	};
	size_t off = 0;
	memcpy(&payload[off], &hdr, sizeof(hdr));
	off += sizeof(hdr);
	memcpy(&payload[off], ssid, ssid_len);
	off += ssid_len;
	if (psk_len > 0u) {
		memcpy(&payload[off], pass, psk_len);
		off += psk_len;
	}
	/* Poll status to connected/failed: the firmware reports BUSY while the
	 * association runs, then OK (connected) or a hard error (auth/no-AP). */
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_WIFI_CONNECT_STA, payload, off, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_wifi_disconnect(cc3501e_t *ctx)
{
	/* Tear down the STA association (WIFI_DISCONNECT, 0x13).  No payload, no
	 * reply data -- success is the OK status.  Disconnect is a radio op
	 * (Wlan_Disconnect), so the bridge can be briefly down (IO/BUSY) while it
	 * runs; floor the budget to the radio down-window and let poll_by_repeat
	 * retry, exactly like cc3501e_wifi_get_mac. */
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_WIFI_DISCONNECT, NULL, 0, NULL, 0, NULL, CC3501E_WIFI_DOWN_WINDOW_MS);
}

alp_status_t cc3501e_wifi_ap_start(
    cc3501e_t *ctx, const char *ssid, uint8_t sec_type, const char *pass, uint32_t timeout_ms)
{
	if (ssid == NULL) return ALP_ERR_INVAL;
	size_t ssid_len = strlen(ssid);
	size_t psk_len  = (pass != NULL) ? strlen(pass) : 0u;
	if (ssid_len > 32u || psk_len > 64u) return ALP_ERR_INVAL;

	/* WIFI_AP_START reuses the STA connect wire format: an
	 * alp_cc3501e_wifi_connect_t header (4 B) + inline SSID + inline
	 * passphrase, all packed with no padding (firmware wifi_join validates
	 * both paths against the same struct). */
	uint8_t                    payload[sizeof(alp_cc3501e_wifi_connect_t) + 32u + 64u];
	alp_cc3501e_wifi_connect_t hdr = {
		.ssid_len = (uint8_t)ssid_len,
		.psk_len  = (uint8_t)psk_len,
		.security = sec_type,
		.reserved = 0u,
	};
	size_t off = 0;
	memcpy(&payload[off], &hdr, sizeof(hdr));
	off += sizeof(hdr);
	memcpy(&payload[off], ssid, ssid_len);
	off += ssid_len;
	if (psk_len > 0u) {
		memcpy(&payload[off], pass, psk_len);
		off += psk_len;
	}
	/* AP bring-up is worker-routed in the firmware, so the bridge is briefly
	 * down (BUSY/IO) while the radio comes up; poll_by_repeat re-issues until
	 * OK (AP up) or a hard error, exactly like cc3501e_wifi_connect. */
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_WIFI_AP_START, payload, off, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_wifi_ap_stop(cc3501e_t *ctx)
{
	/* Tear down the soft-AP (WIFI_AP_STOP, 0x15).  No payload, no reply data --
	 * success is the OK status.  Like cc3501e_wifi_disconnect this is a radio
	 * op, so the bridge can be briefly down (IO/BUSY) while it runs; floor the
	 * budget to the radio down-window and let poll_by_repeat retry. */
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_WIFI_AP_STOP, NULL, 0, NULL, 0, NULL, CC3501E_WIFI_DOWN_WINDOW_MS);
}

alp_status_t cc3501e_wifi_rssi(cc3501e_t *ctx, int8_t *rssi)
{
	if (rssi == NULL) return ALP_ERR_INVAL;
	uint8_t      reply[1] = { 0 };
	size_t       got      = 0;
	alp_status_t s        = cc3501e_request(ctx,
	                                        ALP_CC3501E_CMD_WIFI_GET_RSSI,
	                                        NULL,
	                                        0,
	                                        reply,
	                                        sizeof(reply),
	                                        &got,
	                                        CC3501E_REQ_TMO_MS);
	if (s != ALP_OK) return s;
	if (got < 1u) return ALP_ERR_IO;
	*rssi = (int8_t)reply[0];
	return ALP_OK;
}

alp_status_t cc3501e_wifi_get_ip(cc3501e_t *ctx, uint8_t ip[4])
{
	if (ip == NULL) return ALP_ERR_INVAL;
	uint8_t      reply[4] = { 0 };
	size_t       got      = 0;
	alp_status_t s        = cc3501e_request(
	    ctx, ALP_CC3501E_CMD_WIFI_GET_IP, NULL, 0, reply, sizeof(reply), &got, CC3501E_REQ_TMO_MS);
	if (s != ALP_OK) return s;
	if (got < 4u) return ALP_ERR_IO;
	/* Byte-order normalise (host-only): the firmware derives these 4 bytes from the
	 * lwIP netif address -- a NETWORK-order u32 (netif_ip4_addr()->addr) -- but extracts
	 * it MSB-first, so on the wire the octets arrive REVERSED (192.168.1.14 -> the wire
	 * bytes [14,1,168,192]).  Reverse them here to canonical dotted-quad order
	 * (ip[0]=192 ... ip[3]=14), which is directly printable AND matches the ip[]
	 * convention cc3501e_sock_connect expects (network order, ip[0] = most-significant
	 * octet) -- so a get_ip result can feed straight back into a connect. */
	ip[0] = reply[3];
	ip[1] = reply[2];
	ip[2] = reply[1];
	ip[3] = reply[0];
	return ALP_OK;
}

alp_status_t cc3501e_wifi_status(cc3501e_t *ctx, alp_cc3501e_wifi_status_t *out)
{
	if (out == NULL) return ALP_ERR_INVAL;

	/* Reply is the fixed 4-byte alp_cc3501e_wifi_status_t wire layout (no
	 * padding): state | fail_reason | rssi_dbm | reserved.  The status is a
	 * NON-BLOCKING latch read (no radio op), so -- like cc3501e_wifi_rssi /
	 * cc3501e_wifi_get_ip -- a single request with the short timeout suffices;
	 * no poll-by-repeat down-window handling is needed. */
	uint8_t      reply[4] = { 0 };
	size_t       got      = 0;
	alp_status_t s        = cc3501e_request(
	    ctx, ALP_CC3501E_CMD_WIFI_STATUS, NULL, 0, reply, sizeof(reply), &got, CC3501E_REQ_TMO_MS);
	if (s != ALP_OK) return s;
	if (got < sizeof(reply)) return ALP_ERR_IO; /* short reply -- firmware/wire gap */

	/* Decode wire -> struct field by field (matches the packed layout in
	 * alp/protocol/cc3501e.h), mirroring how cc3501e_wifi_scan walks records. */
	out->state       = reply[0];
	out->fail_reason = reply[1];
	out->rssi_dbm    = (int8_t)reply[2];
	out->reserved    = reply[3];
	return ALP_OK;
}

/* ------------------------------------------------------------------ */
/* TCP/UDP socket host helpers (0x20..0x24)                            */
/*                                                                     */
/* Each wraps cc3501e_request over the packed wire structs in          */
/* <alp/protocol/cc3501e.h>.  The firmware worker-routes every socket   */
/* op (the lwIP bodies block), so each is a poll-by-repeat that re-     */
/* issues the SAME frame while the firmware reports RESP_ERR_BUSY (op   */
/* in flight) or the bridge reads IO (down mid-op), until it resolves.  */
/* v1 IPv4-only; addresses are 4 octets in network order.              */
/* ------------------------------------------------------------------ */

/* Wire header size of alp_cc3501e_sock_send_t (handle | flags | reserved |
 * data_len | reserved2), and of the alp_cc3501e_sock_recv_resp_t reply header
 * (from sock_addr(20) | data_len | reserved).  Fixed by the protocol header. */
#define CC3501E_SOCK_SEND_HDR      8u
#define CC3501E_SOCK_RECV_RESP_HDR 24u

alp_status_t cc3501e_sock_open(cc3501e_t *ctx,
                               uint8_t    family,
                               uint8_t    type,
                               uint8_t    protocol,
                               uint16_t  *handle_out,
                               uint32_t   timeout_ms)
{
	if (handle_out == NULL) return ALP_ERR_INVAL;
	*handle_out = 0u;
	/* SOCK_OPEN (0x20) wire = alp_cc3501e_sock_open_t { family | type | protocol |
	 * reserved }; reply DATA = alp_cc3501e_sock_handle_t { handle(LE16) | rsvd }. */
	uint8_t      payload[4] = { family, type, protocol, 0u };
	uint8_t      reply[4]   = { 0 };
	size_t       got        = 0;
	alp_status_t s          = poll_by_repeat(ctx,
	                                         ALP_CC3501E_CMD_SOCK_OPEN,
	                                         payload,
	                                         sizeof(payload),
	                                         reply,
	                                         sizeof(reply),
	                                         &got,
	                                         timeout_ms);
	if (s != ALP_OK) return s;
	if (got < 2u) return ALP_ERR_IO; /* short reply -- firmware/wire gap */
	*handle_out = (uint16_t)reply[0] | ((uint16_t)reply[1] << 8);
	return ALP_OK;
}

alp_status_t cc3501e_sock_connect(
    cc3501e_t *ctx, uint16_t handle, const uint8_t ip[4], uint16_t port, uint32_t timeout_ms)
{
	if (ip == NULL) return ALP_ERR_INVAL;
	/* SOCK_CONNECT (0x21) wire = alp_cc3501e_sock_connect_t: handle(LE16) |
	 * reserved(2) | peer sock_addr { family | reserved | port(LE16) | addr[16] }. */
	uint8_t p[24];
	memset(p, 0, sizeof(p));
	p[0] = (uint8_t)(handle & 0xFFu);
	p[1] = (uint8_t)((handle >> 8) & 0xFFu);
	p[4] = (uint8_t)ALP_CC3501E_SOCK_FAMILY_IPV4; /* peer.family */
	p[6] = (uint8_t)(port & 0xFFu);               /* peer.port (LE16, host order) */
	p[7] = (uint8_t)((port >> 8) & 0xFFu);
	memcpy(&p[8], ip, 4); /* peer.addr[0..3]; addr[4..15] stay zero (IPv4) */
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_SOCK_CONNECT, p, sizeof(p), NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_sock_send(cc3501e_t     *ctx,
                               uint16_t       handle,
                               const uint8_t *data,
                               size_t         len,
                               size_t        *sent_out,
                               uint32_t       timeout_ms)
{
	if (data == NULL && len > 0u) return ALP_ERR_INVAL;
	if (len > (size_t)(ALP_CC3501E_MAX_PAYLOAD - CC3501E_SOCK_SEND_HDR)) return ALP_ERR_INVAL;
	if (sent_out != NULL) *sent_out = 0u;

	/* SOCK_SEND (0x22) wire = alp_cc3501e_sock_send_t (8 B) + inline data; reply
	 * DATA = uint16_t LE queued-byte count. */
	uint8_t p[ALP_CC3501E_MAX_PAYLOAD];
	p[0] = (uint8_t)(handle & 0xFFu);
	p[1] = (uint8_t)((handle >> 8) & 0xFFu);
	p[2] = 0u; /* flags (MORE bit unused here) */
	p[3] = 0u;
	p[4] = (uint8_t)(len & 0xFFu);
	p[5] = (uint8_t)((len >> 8) & 0xFFu);
	p[6] = 0u;
	p[7] = 0u;
	if (len > 0u) memcpy(&p[CC3501E_SOCK_SEND_HDR], data, len);

	uint8_t      reply[2] = { 0 };
	size_t       got      = 0;
	alp_status_t s        = poll_by_repeat(ctx,
	                                       ALP_CC3501E_CMD_SOCK_SEND,
	                                       p,
	                                       CC3501E_SOCK_SEND_HDR + len,
	                                       reply,
	                                       sizeof(reply),
	                                       &got,
	                                       timeout_ms);
	if (s != ALP_OK) return s;
	if (sent_out != NULL && got >= 2u) {
		*sent_out = (size_t)((uint16_t)reply[0] | ((uint16_t)reply[1] << 8));
	}
	return ALP_OK;
}

alp_status_t cc3501e_sock_recv(cc3501e_t *ctx,
                               uint16_t   handle,
                               uint8_t   *buf,
                               size_t     cap,
                               size_t    *recv_len_out,
                               uint32_t   timeout_ms)
{
	if (buf == NULL && cap > 0u) return ALP_ERR_INVAL;
	if (recv_len_out != NULL) *recv_len_out = 0u;

	/* Bound the requested count so the reply (recv_resp header + data + status)
	 * fits one frame. */
	size_t       want     = cap;
	const size_t want_max = (size_t)(ALP_CC3501E_MAX_PAYLOAD - CC3501E_SOCK_RECV_RESP_HDR - 1u);
	if (want > want_max) want = want_max;

	/* SOCK_RECV (0x23) wire = alp_cc3501e_sock_recv_t { handle(LE16) | max_len(LE16) }. */
	uint8_t p[4] = { (uint8_t)(handle & 0xFFu),
		             (uint8_t)((handle >> 8) & 0xFFu),
		             (uint8_t)(want & 0xFFu),
		             (uint8_t)((want >> 8) & 0xFFu) };

	uint8_t      reply[ALP_CC3501E_MAX_PAYLOAD];
	size_t       got = 0;
	alp_status_t s   = poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_SOCK_RECV, p, sizeof(p), reply, sizeof(reply), &got, timeout_ms);
	if (s != ALP_OK) return s;
	if (got < CC3501E_SOCK_RECV_RESP_HDR) return ALP_ERR_IO; /* short reply header */

	/* recv_resp header: from sock_addr(20) | data_len(LE16 @20) | reserved(@22).
	 * The received bytes follow inline at offset 24. */
	size_t data_len = (size_t)((uint16_t)reply[20] | ((uint16_t)reply[21] << 8));
	if (CC3501E_SOCK_RECV_RESP_HDR + data_len > got) {
		data_len = got - CC3501E_SOCK_RECV_RESP_HDR; /* truncated -- clamp to captured */
	}
	size_t copy = (data_len > cap) ? cap : data_len;
	if (copy > 0u) memcpy(buf, &reply[CC3501E_SOCK_RECV_RESP_HDR], copy);
	if (recv_len_out != NULL) *recv_len_out = copy;
	return ALP_OK;
}

alp_status_t cc3501e_sock_close(cc3501e_t *ctx, uint16_t handle, uint32_t timeout_ms)
{
	/* SOCK_CLOSE (0x24) wire = alp_cc3501e_sock_close_t { handle(LE16) | reserved }. */
	uint8_t p[4] = { (uint8_t)(handle & 0xFFu), (uint8_t)((handle >> 8) & 0xFFu), 0u, 0u };
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_SOCK_CLOSE, p, sizeof(p), NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ble_enable(cc3501e_t *ctx, uint32_t timeout_ms)
{
	/* Worker-routed: the firmware SUSPENDS the bridge SPI, runs BleIf_EnableBLE (the
	 * NWP BLE-controller cold-init -- a control-cmd round-trip that can take ~10-15 s)
	 * + nimble_host_start sync, then RE-OPENS the bridge.  The link is DOWN for that
	 * whole window, so the host must poll-by-repeat (retry on IO) longer than the
	 * 10 s Wi-Fi floor -- floor to 30 s so a working-but-slow enable is not misread as
	 * a failure before the worker publishes the result + the bridge re-syncs. */
	uint32_t budget = timeout_ms;
	if (budget < CC3501E_BLE_ENABLE_WINDOW_MS) budget = CC3501E_BLE_ENABLE_WINDOW_MS;
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_BLE_ENABLE, NULL, 0, NULL, 0, NULL, budget);
}

/* Wire BLE scan record: addr[6] | addr_type | rssi(int8) | name_len | name[name_len]
 * (see cc3501e_ble_scan_record_t). Fixed 9-byte header, name packed inline. */
#define CC3501E_BLE_REC_HDR 9u

alp_status_t cc3501e_ble_scan(cc3501e_t                 *ctx,
                              cc3501e_ble_scan_record_t *out_records,
                              size_t                     cap,
                              size_t                    *count,
                              uint32_t                   timeout_ms)
{
	if (out_records == NULL && cap > 0u) return ALP_ERR_INVAL;
	if (count != NULL) *count = 0;

	/* Mirror of cc3501e_wifi_scan: the firmware returns the advertising
	 * reports it collected as the BLE_SCAN_START reply payload. */
	static uint8_t scan_buf[ALP_CC3501E_MAX_PAYLOAD];
	size_t         got = 0;
	alp_status_t   s   = poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_BLE_SCAN_START, NULL, 0, scan_buf, sizeof(scan_buf), &got, timeout_ms);
	if (s != ALP_OK) return s;

	size_t off = 0;
	size_t n   = 0;
	while (off + CC3501E_BLE_REC_HDR <= got && n < cap) {
		const uint8_t *rec      = &scan_buf[off];
		uint8_t        name_len = rec[8];
		if (off + CC3501E_BLE_REC_HDR + (size_t)name_len > got) {
			break; /* truncated trailing record -- stop cleanly */
		}
		cc3501e_ble_scan_record_t *out = &out_records[n];
		memcpy(out->addr, &rec[0], 6);
		out->addr_type = rec[6];
		out->rssi_dbm  = (int8_t)rec[7];
		out->name_len  = name_len;
		uint8_t copy = (name_len > CC3501E_BLE_NAME_MAX) ? (uint8_t)CC3501E_BLE_NAME_MAX : name_len;
		memcpy(out->name, &rec[CC3501E_BLE_REC_HDR], copy);
		out->name[copy] = '\0';
		off += CC3501E_BLE_REC_HDR + (size_t)name_len;
		n++;
	}
	if (count != NULL) *count = n;
	return ALP_OK;
}

/* ------------------------------------------------------------------ */
/* BLE control (0x31..0x3B).                                          */
/*                                                                    */
/* Thin wrappers over cc3501e_request matching the firmware BLE        */
/* handlers (firmware/cc3501e/src/protocol.c handle_ble_*).  WIRE GAP: */
/* the protocol header carries the BLE opcodes + alp_cc3501e_ble_adv_  */
/* start_t, but has NO payload struct for CONNECT (0x36) or the four   */
/* GATT ops (0x38..0x3B); those layouts are defined only by the        */
/* firmware handlers and are transcribed per-helper below (the same    */
/* precedent as the Wi-Fi GET_MAC / scan wire-gap notes above).  These */
/* ops are direct (non-worker-routed) in the firmware, so they take    */
/* the caller's timeout; poll_by_repeat still absorbs a transient      */
/* ALP_ERR_IO if a radio op happens to overlap (bridge briefly down).  */
/* ------------------------------------------------------------------ */

alp_status_t cc3501e_ble_disable(cc3501e_t *ctx, uint32_t timeout_ms)
{
	/* BLE_DISABLE (0x31): no payload (firmware handle_ble_disable rejects any
	 * non-empty body).  No reply data -- success is the OK status. */
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_BLE_DISABLE, NULL, 0, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ble_adv_start(cc3501e_t     *ctx,
                                   bool           connectable,
                                   uint16_t       interval_min_ms,
                                   uint16_t       interval_max_ms,
                                   const uint8_t *adv_data,
                                   uint8_t        adv_data_len,
                                   uint32_t       timeout_ms)
{
	if (adv_data == NULL && adv_data_len > 0u) return ALP_ERR_INVAL;

	/* Hand-pack the 7-byte wire header (firmware handle_ble_adv_start
	 * BLE_ADV_START_HDR), NOT the doc struct alp_cc3501e_ble_adv_start_t which is
	 * 8 bytes with a u16-alignment pad the wire omits:
	 *   connectable(1) | reserved(1,=0) | interval_min_ms(LE16) |
	 *   interval_max_ms(LE16) | adv_data_len(1) | adv_data[adv_data_len]. */
	uint8_t buf[7u + 255u];
	buf[0] = connectable ? 1u : 0u;
	buf[1] = 0u;
	buf[2] = (uint8_t)(interval_min_ms & 0xFFu);
	buf[3] = (uint8_t)((interval_min_ms >> 8) & 0xFFu);
	buf[4] = (uint8_t)(interval_max_ms & 0xFFu);
	buf[5] = (uint8_t)((interval_max_ms >> 8) & 0xFFu);
	buf[6] = adv_data_len;
	if (adv_data_len > 0u) {
		memcpy(&buf[7], adv_data, adv_data_len);
	}
	return poll_by_repeat(ctx,
	                      ALP_CC3501E_CMD_BLE_ADV_START,
	                      buf,
	                      (size_t)7u + adv_data_len,
	                      NULL,
	                      0,
	                      NULL,
	                      timeout_ms);
}

alp_status_t cc3501e_ble_adv_stop(cc3501e_t *ctx, uint32_t timeout_ms)
{
	/* BLE_ADV_STOP (0x33): no payload. */
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_BLE_ADV_STOP, NULL, 0, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ble_scan_stop(cc3501e_t *ctx, uint32_t timeout_ms)
{
	/* BLE_SCAN_STOP (0x35): no payload. */
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_BLE_SCAN_STOP, NULL, 0, NULL, 0, NULL, timeout_ms);
}

alp_status_t
cc3501e_ble_connect(cc3501e_t *ctx, const uint8_t addr[6], uint8_t addr_type, uint32_t timeout_ms)
{
	if (addr == NULL) return ALP_ERR_INVAL;
	/* BLE_CONNECT (0x36) wire (firmware handle_ble_connect, req_len == 7):
	 * addr_type(1) | addr[6].  NOTE the addr_type-FIRST order.  No header struct
	 * -- the layout is defined only by the firmware handler. */
	uint8_t req[7];
	req[0] = addr_type;
	memcpy(&req[1], addr, 6);
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_BLE_CONNECT, req, sizeof(req), NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ble_disconnect(cc3501e_t *ctx, uint32_t timeout_ms)
{
	/* BLE_DISCONNECT (0x37): no payload (firmware handle_ble_disconnect rejects
	 * any body -- the firmware tracks the single active connection itself). */
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_BLE_DISCONNECT, NULL, 0, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ble_gatt_register(cc3501e_t     *ctx,
                                       const uint8_t *descriptor,
                                       size_t         len,
                                       uint32_t       timeout_ms)
{
	if (descriptor == NULL || len == 0u || len > ALP_CC3501E_MAX_PAYLOAD) return ALP_ERR_INVAL;
	/* BLE_GATT_REGISTER (0x38): the firmware handle_ble_gatt_register takes the
	 * payload as an OPAQUE attribute-table descriptor (>= 1 byte) -- there is no
	 * header struct and no fixed UUID/handle layout on the host side; the host
	 * ships the descriptor bytes verbatim and the firmware parses them. */
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_BLE_GATT_REGISTER, descriptor, len, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ble_gatt_notify(
    cc3501e_t *ctx, uint16_t handle, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
	if (data == NULL && len > 0u) return ALP_ERR_INVAL;
	if (len > (size_t)(ALP_CC3501E_MAX_PAYLOAD - 2u)) return ALP_ERR_INVAL;
	/* BLE_GATT_NOTIFY (0x39) wire (firmware handle_ble_gatt_notify): handle(LE16)
	 * | value bytes.  No header struct -- layout from the firmware handler. */
	uint8_t buf[2u + (ALP_CC3501E_MAX_PAYLOAD - 2u)];
	buf[0] = (uint8_t)(handle & 0xFFu);
	buf[1] = (uint8_t)((handle >> 8) & 0xFFu);
	if (len > 0u) memcpy(&buf[2], data, len);
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_BLE_GATT_NOTIFY, buf, 2u + len, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ble_gatt_read(
    cc3501e_t *ctx, uint16_t handle, uint8_t *out, size_t cap, size_t *out_len, uint32_t timeout_ms)
{
	if (out == NULL && cap > 0u) return ALP_ERR_INVAL;
	if (out_len != NULL) *out_len = 0;
	/* BLE_GATT_READ (0x3A) wire (firmware handle_ble_gatt_read, req_len == 2):
	 * request = handle(LE16); the reply DATA (after the status byte) is the
	 * attribute value bytes.  No header struct -- layout from the firmware
	 * handler.  poll_by_repeat copies the value into @out (capped at @cap). */
	uint8_t req[2];
	req[0] = (uint8_t)(handle & 0xFFu);
	req[1] = (uint8_t)((handle >> 8) & 0xFFu);
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_BLE_GATT_READ, req, sizeof(req), out, cap, out_len, timeout_ms);
}

alp_status_t cc3501e_ble_gatt_write(
    cc3501e_t *ctx, uint16_t handle, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
	if (data == NULL && len > 0u) return ALP_ERR_INVAL;
	if (len > (size_t)(ALP_CC3501E_MAX_PAYLOAD - 2u)) return ALP_ERR_INVAL;
	/* BLE_GATT_WRITE (0x3B) wire (firmware handle_ble_gatt_write): handle(LE16) |
	 * value bytes -- identical framing to NOTIFY. */
	uint8_t buf[2u + (ALP_CC3501E_MAX_PAYLOAD - 2u)];
	buf[0] = (uint8_t)(handle & 0xFFu);
	buf[1] = (uint8_t)((handle >> 8) & 0xFFu);
	if (len > 0u) memcpy(&buf[2], data, len);
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_BLE_GATT_WRITE, buf, 2u + len, NULL, 0, NULL, timeout_ms);
}

/* ------------------------------------------------------------------ */
/* Meta + diagnostics host helpers (0x00 / 0x02 / 0x04 / 0x70 / 0x71). */
/*                                                                    */
/* All backend-independent: the firmware answers these from in-RAM    */
/* bookkeeping (no radio op), so they take the caller's short request  */
/* budget with no radio-down-window floor.  Reply layouts are decoded  */
/* field-by-field from <alp/protocol/cc3501e.h>.                       */
/* ------------------------------------------------------------------ */

alp_status_t cc3501e_ping(cc3501e_t *ctx)
{
	/* PING (0x00): header-only request, no reply data -- the OK status IS the
	 * liveness proof.  Distinct from cc3501e_get_version (which round-trips the
	 * protocol version): PING is the cheapest is-the-firmware-alive probe. */
	return cc3501e_request(ctx, ALP_CC3501E_CMD_PING, NULL, 0, NULL, 0, NULL, CC3501E_REQ_TMO_MS);
}

alp_status_t cc3501e_soft_reset(cc3501e_t *ctx)
{
	/* RESET (0x02): the firmware ACKs, then performs a DEFERRED reboot (it lets
	 * the reply drain before resetting).  Unlike cc3501e_reset / cc3501e_hard_reset
	 * -- which pulse the WIFI_EN / nRESET GPIOs -- this is an in-band firmware-side
	 * reboot needing no host pins.  The link drops after the ack; the caller
	 * re-syncs (cc3501e_sync) + re-pings once the firmware is back up. */
	return cc3501e_request(ctx, ALP_CC3501E_CMD_RESET, NULL, 0, NULL, 0, NULL, CC3501E_REQ_TMO_MS);
}

alp_status_t cc3501e_diag_info(cc3501e_t *ctx, alp_cc3501e_diag_info_t *out)
{
	if (out == NULL) return ALP_ERR_INVAL;
	/* GET_DIAG_INFO (0x04) reply = the 16-byte packed alp_cc3501e_diag_info_t:
	 * fw_version(LE16) | reset_cause(1) | role(1) | uptime_ms(LE32) |
	 * free_heap_bytes(LE32) | last_error(1) | reserved(3). */
	uint8_t      reply[16] = { 0 };
	size_t       got       = 0;
	alp_status_t s         = cc3501e_request(ctx,
	                                         ALP_CC3501E_CMD_GET_DIAG_INFO,
	                                         NULL,
	                                         0,
	                                         reply,
	                                         sizeof(reply),
	                                         &got,
	                                         CC3501E_REQ_TMO_MS);
	if (s != ALP_OK) return s;
	if (got < sizeof(reply)) return ALP_ERR_IO;
	out->fw_version  = (uint16_t)reply[0] | ((uint16_t)reply[1] << 8);
	out->reset_cause = reply[2];
	out->role        = reply[3];
	out->uptime_ms   = (uint32_t)reply[4] | ((uint32_t)reply[5] << 8) | ((uint32_t)reply[6] << 16) |
	                   ((uint32_t)reply[7] << 24);
	out->free_heap_bytes = (uint32_t)reply[8] | ((uint32_t)reply[9] << 8) |
	                       ((uint32_t)reply[10] << 16) | ((uint32_t)reply[11] << 24);
	out->last_error      = reply[12];
	out->reserved[0]     = reply[13];
	out->reserved[1]     = reply[14];
	out->reserved[2]     = reply[15];
	return ALP_OK;
}

alp_status_t cc3501e_diag_stats(cc3501e_t *ctx, uint32_t *frames_ok, uint32_t *frames_err)
{
	if (frames_ok == NULL || frames_err == NULL) return ALP_ERR_INVAL;
	/* DIAG_GET_STATS (0x70) reply = frames_ok(LE32) | frames_err(LE32).  The
	 * protocol header carries the opcode but NO reply struct for these two frame
	 * counters, so they are returned via out-params rather than a typedef. */
	uint8_t      reply[8] = { 0 };
	size_t       got      = 0;
	alp_status_t s        = cc3501e_request(ctx,
	                                        ALP_CC3501E_CMD_DIAG_GET_STATS,
	                                        NULL,
	                                        0,
	                                        reply,
	                                        sizeof(reply),
	                                        &got,
	                                        CC3501E_REQ_TMO_MS);
	if (s != ALP_OK) return s;
	if (got < sizeof(reply)) return ALP_ERR_IO;
	*frames_ok  = (uint32_t)reply[0] | ((uint32_t)reply[1] << 8) | ((uint32_t)reply[2] << 16) |
	              ((uint32_t)reply[3] << 24);
	*frames_err = (uint32_t)reply[4] | ((uint32_t)reply[5] << 8) | ((uint32_t)reply[6] << 16) |
	              ((uint32_t)reply[7] << 24);
	return ALP_OK;
}

alp_status_t cc3501e_diag_log_level(cc3501e_t *ctx, uint8_t level)
{
	/* DIAG_LOG_LEVEL (0x71): request payload = level(1); no reply data. */
	uint8_t payload[1] = { level };
	return cc3501e_request(ctx,
	                       ALP_CC3501E_CMD_DIAG_LOG_LEVEL,
	                       payload,
	                       sizeof(payload),
	                       NULL,
	                       0,
	                       NULL,
	                       CC3501E_REQ_TMO_MS);
}

/* ------------------------------------------------------------------ */
/* GPIO proxy (0x50..0x53) + camera enables (0x60/0x61).              */
/*                                                                    */
/* These ops are synchronous + fast in the firmware (no worker, no    */
/* radio bring-up), so they take the caller's timeout with no down-   */
/* window floor.  poll_by_repeat still absorbs a transient ALP_ERR_IO  */
/* if a radio op happens to overlap (the bridge is briefly down then). */
/* pad = the RAW CC3501E GPIO index; the firmware drives it 1:1 and    */
/* refuses its own SPI/UART pads, so the logical IO11.. -> raw map can  */
/* live entirely host-side in board metadata. */
/* ------------------------------------------------------------------ */

alp_status_t cc3501e_gpio_configure(cc3501e_t                   *ctx,
                                    uint8_t                      pad,
                                    alp_cc3501e_gpio_direction_t dir,
                                    alp_cc3501e_gpio_pull_t      pull,
                                    uint32_t                     timeout_ms)
{
	alp_cc3501e_gpio_configure_t c = {
		.cc3501e_gpio = pad,
		.direction    = (uint8_t)dir,
		.pull         = (uint8_t)pull,
		.reserved     = 0u,
	};
	return poll_by_repeat(ctx,
	                      ALP_CC3501E_CMD_GPIO_CONFIGURE,
	                      (const uint8_t *)&c,
	                      sizeof(c),
	                      NULL,
	                      0,
	                      NULL,
	                      timeout_ms);
}

alp_status_t cc3501e_gpio_write(cc3501e_t *ctx, uint8_t pad, bool level, uint32_t timeout_ms)
{
	alp_cc3501e_gpio_write_t w = {
		.cc3501e_gpio = pad,
		.level        = level ? 1u : 0u,
		.reserved     = { 0u, 0u },
	};
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_GPIO_WRITE, (const uint8_t *)&w, sizeof(w), NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_gpio_read(cc3501e_t *ctx, uint8_t pad, bool *level_out, uint32_t timeout_ms)
{
	if (level_out == NULL) return ALP_ERR_INVAL;
	uint8_t      req      = pad;
	uint8_t      reply[1] = { 0 };
	size_t       got      = 0;
	alp_status_t s        = poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_GPIO_READ, &req, sizeof(req), reply, sizeof(reply), &got, timeout_ms);
	if (s != ALP_OK) return s;
	if (got < 1u) return ALP_ERR_IO;
	*level_out = (reply[0] != 0u);
	return ALP_OK;
}

alp_status_t cc3501e_gpio_set_interrupt(
    cc3501e_t *ctx, uint8_t pad, alp_cc3501e_gpio_edge_t edge, bool enabled, uint32_t timeout_ms)
{
	alp_cc3501e_gpio_set_interrupt_t s = {
		.cc3501e_gpio = pad,
		.edge         = (uint8_t)edge,
		.enabled      = enabled ? 1u : 0u,
		.reserved     = 0u,
	};
	return poll_by_repeat(ctx,
	                      ALP_CC3501E_CMD_GPIO_SET_INTERRUPT,
	                      (const uint8_t *)&s,
	                      sizeof(s),
	                      NULL,
	                      0,
	                      NULL,
	                      timeout_ms);
}

alp_status_t cc3501e_cam_enable(cc3501e_t *ctx, uint8_t which, bool on, uint32_t timeout_ms)
{
	uint8_t           req = which;
	alp_cc3501e_cmd_t cmd = on ? ALP_CC3501E_CMD_CAM_ENABLE : ALP_CC3501E_CMD_CAM_DISABLE;
	return poll_by_repeat(ctx, cmd, &req, sizeof(req), NULL, 0, NULL, timeout_ms);
}

alp_status_t
cc3501e_power_policy(cc3501e_t *ctx, const alp_cc3501e_power_policy_t *policy, uint32_t timeout_ms)
{
	if (policy == NULL) return ALP_ERR_INVAL;
	/* Pack the 8-byte wire by hand (NOT via the doc struct, which carries u16/u32
	 * alignment padding the wire does not) -- byte-for-byte what handle_power_policy
	 * parses: policy(1) | wake_events(1) | reserved(2,=0) | idle_ms_before_sleep(LE32). */
	const uint32_t idle = policy->idle_ms_before_sleep;
	uint8_t        req[8];
	req[0] = policy->policy;
	req[1] = policy->wake_events;
	req[2] = 0u;
	req[3] = 0u;
	req[4] = (uint8_t)(idle & 0xFFu);
	req[5] = (uint8_t)((idle >> 8) & 0xFFu);
	req[6] = (uint8_t)((idle >> 16) & 0xFFu);
	req[7] = (uint8_t)((idle >> 24) & 0xFFu);
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_POWER_POLICY, req, sizeof(req), NULL, 0, NULL, timeout_ms);
}

/* ---- OTA firmware update (stream a new image over the bridge) --------------- */

alp_status_t cc3501e_ota_begin(cc3501e_t *ctx, uint32_t total_len, uint32_t timeout_ms)
{
	uint8_t req[4];
	req[0] = (uint8_t)(total_len & 0xFFu);
	req[1] = (uint8_t)((total_len >> 8) & 0xFFu);
	req[2] = (uint8_t)((total_len >> 16) & 0xFFu);
	req[3] = (uint8_t)((total_len >> 24) & 0xFFu);
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_OTA_BEGIN, req, sizeof(req), NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ota_write(
    cc3501e_t *ctx, uint32_t offset, const uint8_t *data, size_t len, uint32_t timeout_ms)
{
	if (data == NULL || len == 0u || len > ALP_CC3501E_OTA_MAX_CHUNK) {
		return ALP_ERR_INVAL;
	}
	/* Frame = offset(LE32) followed by the raw image bytes (<= MAX_PAYLOAD). */
	uint8_t buf[4u + ALP_CC3501E_OTA_MAX_CHUNK];
	buf[0] = (uint8_t)(offset & 0xFFu);
	buf[1] = (uint8_t)((offset >> 8) & 0xFFu);
	buf[2] = (uint8_t)((offset >> 16) & 0xFFu);
	buf[3] = (uint8_t)((offset >> 24) & 0xFFu);
	memcpy(&buf[4], data, len);
	/* The device stages each chunk synchronously into RAM (no flash until FINISH),
	 * so a WRITE neither blocks nor disrupts the bridge -- a plain re-send poll is
	 * safe + fast (the device is idempotent on the cursor).  ALL the OTA flash, and
	 * thus the only bridge-DMA-disruption window, is FINISH. */
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_OTA_WRITE, buf, 4u + len, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ota_finish(cc3501e_t *ctx, uint32_t timeout_ms)
{
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_OTA_FINISH, NULL, 0, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ota_abort(cc3501e_t *ctx, uint32_t timeout_ms)
{
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_OTA_ABORT, NULL, 0, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ota_status(cc3501e_t *ctx, alp_cc3501e_ota_status_t *out, uint32_t timeout_ms)
{
	if (out == NULL) return ALP_ERR_INVAL;
	uint8_t      reply[12] = { 0 };
	size_t       got       = 0;
	alp_status_t s         = poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_OTA_STATUS, NULL, 0, reply, sizeof(reply), &got, timeout_ms);
	if (s != ALP_OK) return s;
	if (got < sizeof(reply)) return ALP_ERR_IO;
	out->state         = reply[0];
	out->reserved[0]   = reply[1];
	out->reserved[1]   = reply[2];
	out->reserved[2]   = reply[3];
	out->bytes_written = (uint32_t)reply[4] | ((uint32_t)reply[5] << 8) |
	                     ((uint32_t)reply[6] << 16) | ((uint32_t)reply[7] << 24);
	out->total_len = (uint32_t)reply[8] | ((uint32_t)reply[9] << 8) | ((uint32_t)reply[10] << 16) |
	                 ((uint32_t)reply[11] << 24);
	return ALP_OK;
}

alp_status_t
cc3501e_ota_update(cc3501e_t *ctx, const uint8_t *image, size_t len, uint32_t timeout_ms)
{
	if (image == NULL || len == 0u) return ALP_ERR_INVAL;

	alp_status_t s = cc3501e_ota_begin(ctx, (uint32_t)len, timeout_ms);
	if (s != ALP_OK) return s;

	/* 256 B = the CC35 flash page / psa_fwu_write granularity (the validated
	 * SELFTEST installer used CC3501E_OTA_WRITE_CHUNK 256).  Non-page-sized
	 * chunks make the device psa_fwu_write fail -> the host loops on IO until the
	 * per-frame timeout (silicon 2026-06-19).  Keep host chunks page-aligned.
	 * (The final remainder chunk is < 256 B; psa_fwu accepts the partial tail,
	 * as the selftest's last write did.) */
	const size_t chunk = 256u;
	for (size_t off = 0u; off < len;) {
		size_t n = len - off;
		if (n > chunk) {
			n = chunk;
		}
		s = cc3501e_ota_write(ctx, (uint32_t)off, image + off, n, timeout_ms);
		if (s != ALP_OK) {
			/* A lost reply can leave the host unsure whether the chunk landed.
			 * OTA_WRITE is NOT idempotent (a re-sent already-written offset is
			 * rejected as out-of-order), so re-sync to the device's actual write
			 * cursor before deciding: if it already advanced past this chunk the
			 * write took -- continue; otherwise abort + report. */
			alp_cc3501e_ota_status_t st;
			if (cc3501e_ota_status(ctx, &st, timeout_ms) == ALP_OK &&
			    st.state == ALP_CC3501E_OTA_STATE_WRITING &&
			    st.bytes_written == (uint32_t)(off + n)) {
				/* chunk landed; the reply was lost -- proceed. */
			} else {
				(void)cc3501e_ota_abort(ctx, timeout_ms);
				return s;
			}
		}
		off += n;
	}

	return cc3501e_ota_finish(ctx, timeout_ms);
}

alp_status_t cc3501e_set_event_callback(cc3501e_t *ctx, cc3501e_event_cb_t cb, void *user)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	ctx->event_cb   = cb;
	ctx->event_user = user;
	return ALP_OK;
}

alp_status_t cc3501e_poll_events(cc3501e_t *ctx)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	/* No sink registered -> don't drain: leave the events queued in the firmware
	 * ring so they aren't lost before a callback is attached (the firmware drains
	 * on every GET_PENDING_EVENTS, so a poll with no cb would silently discard). */
	if (ctx->event_cb == NULL) return ALP_OK;

	/* Drain the firmware event ring (CMD_GET_PENDING_EVENTS, opcode 0x05).  The
	 * reply DATA is a packed list of { evt_opcode(1) | len(1) | payload[len] }
	 * entries; walk them and hand each to the registered callback.  A single
	 * request (fast, non-blocking firmware handler); a transient bridge-down IO
	 * simply surfaces to the caller, which retries on its next poll cycle. */
	static uint8_t evt_buf[ALP_CC3501E_MAX_PAYLOAD];
	size_t         got = 0;
	alp_status_t   s   = cc3501e_request(ctx,
	                                     ALP_CC3501E_CMD_GET_PENDING_EVENTS,
	                                     NULL,
	                                     0,
	                                     evt_buf,
	                                     sizeof(evt_buf),
	                                     &got,
	                                     CC3501E_REQ_TMO_MS);
	if (s != ALP_OK) return s;

	size_t off = 0;
	while (off + ALP_CC3501E_EVENT_HDR_BYTES <= got) {
		const uint8_t opcode = evt_buf[off];
		const uint8_t len    = evt_buf[off + 1u];
		if (off + ALP_CC3501E_EVENT_HDR_BYTES + (size_t)len > got) {
			break; /* truncated trailing entry -- stop cleanly */
		}
		ctx->event_cb(opcode, &evt_buf[off + ALP_CC3501E_EVENT_HDR_BYTES], len, ctx->event_user);
		off += ALP_CC3501E_EVENT_HDR_BYTES + (size_t)len;
	}
	return ALP_OK;
}

void cc3501e_deinit(cc3501e_t *ctx)
{
	if (ctx == NULL) return;
	ctx->initialised = false;
	ctx->bus         = NULL;
}
