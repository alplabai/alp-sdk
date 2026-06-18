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
	(void)alp_gpio_write(ctx->reset_pin, true);  /* release -> re-boot */

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

/* Bench debug (2026-06-16, REVERT after the SPI1-no-clock root cause is found):
 * which transfer step of the last request failed (1=req hdr, 2=req payload,
 * 3=reply hdr, 4=reply payload, 5=reply-len sanity, 6=status byte); 0 = ok. */
volatile int cc3501e_dbg_fail_step;

/* Bench debug (2026-06-16, REVERT): raw MISO/POCI bytes captured during a
 * request, surfaced via the witness so a J-Link reads what the CC3501E drives
 * with no console.  reqhdr_rx = the 4 bytes clocked back WHILE the host writes
 * the request header (step 1); reply_hdr = the 4 bytes read as the reply header
 * (step 3).  Both 0xFFFFFFFF => MISO idle-high / slave not driving; both same
 * non-FF => stuck; differing/structured => slave alive (then it's framing). */
volatile uint32_t cc3501e_dbg_reqhdr_rx;
volatile uint32_t cc3501e_dbg_reply_hdr;

static uint32_t pack4(const uint8_t *b)
{
	return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
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
	const uint32_t walk_max  = 2u * (uint32_t)(ALP_CC3501E_HEADER_BYTES + ALP_CC3501E_MAX_PAYLOAD);
	const uint32_t run_need  = 2u * (uint32_t)ALP_CC3501E_HEADER_BYTES;
	const uint32_t attempts  = (timeout_ms > 0u) ? timeout_ms : 1u;

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

alp_status_t cc3501e_request(cc3501e_t        *ctx,
                             alp_cc3501e_cmd_t cmd,
                             const uint8_t    *tx_payload,
                             size_t            tx_len,
                             uint8_t          *rx_buf,
                             size_t            rx_cap,
                             size_t           *rx_len,
                             uint32_t          timeout_ms)
{
	(void)timeout_ms;          /* Reserved for a future IRQ-driven wait (next HW rev). */
	cc3501e_dbg_fail_step = 0; /* bench debug (REVERT) */
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
	encode_header(ctx->tx_scratch, cmd, ALP_CC3501E_FLAG_RESP_REQUIRED, (uint16_t)tx_len);
	alp_status_t s =
	    alp_spi_transceive(ctx->bus, ctx->tx_scratch, ctx->rx_scratch, ALP_CC3501E_HEADER_BYTES);
	if (s != ALP_OK) {
		cc3501e_dbg_fail_step = 1;
		return s;
	} /* bench debug (REVERT) */
	cc3501e_dbg_reqhdr_rx =
	    pack4(ctx->rx_scratch); /* bench debug (REVERT): MISO during req-hdr write */
	if (tx_len > 0) {
		s = alp_spi_transceive(ctx->bus, tx_payload, ctx->rx_scratch, tx_len);
		if (s != ALP_OK) {
			cc3501e_dbg_fail_step = 2;
			return s;
		} /* bench debug (REVERT) */
	}

	/* Settle gap: let the slave dispatch + arm its reply before we read.
     * v0.1 META dispatch is instant; slow (Wi-Fi/BLE) replies + async
     * events need the next-rev host IRQ line, not a fixed gap. */
	alp_delay_us(200u);

	/* Dummies for the read transactions (MOSI is don't-care on a read). */
	memset(ctx->tx_scratch, 0xFF, sizeof(ctx->tx_scratch));

	/* 3. Reply header -> learn the reply payload length. */
	s = alp_spi_transceive(ctx->bus, ctx->tx_scratch, ctx->rx_scratch, ALP_CC3501E_HEADER_BYTES);
	if (s != ALP_OK) {
		cc3501e_dbg_fail_step = 3;
		return s;
	} /* bench debug (REVERT) */
	cc3501e_dbg_reply_hdr =
	    pack4(ctx->rx_scratch); /* bench debug (REVERT): raw reply header bytes */
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
		cc3501e_dbg_fail_step = 5; /* bench debug (REVERT) */
		/* Do NOT byte-walk cc3501e_sync() here: on the 4-byte fixed-count lockstep
		 * the 1-byte walk PARKS the slave (proven on silicon -- reply_hdr stuck at
		 * 0xA5A5A5A5, link never recovers).  Return IO so the caller re-issues a
		 * clean 4-byte transaction instead (re-aligns when the slave is aligned). */
		return ALP_ERR_IO;
	}

	/* 4. Reply payload: status byte followed by the response data. */
	s = alp_spi_transceive(ctx->bus, ctx->tx_scratch, ctx->rx_scratch, resp_payload_len);
	if (s != ALP_OK) {
		cc3501e_dbg_fail_step = 4;
		return s;
	} /* bench debug (REVERT) */

	const uint8_t resp     = ctx->rx_scratch[0];
	const size_t  data_len = (size_t)resp_payload_len - 1u;
	if (data_len > 0u && rx_buf != NULL) {
		const size_t n = (data_len > rx_cap) ? rx_cap : data_len;
		memcpy(rx_buf, &ctx->rx_scratch[1], n);
		if (rx_len != NULL) *rx_len = n;
	}
	alp_status_t rs = resp_to_status(resp);
	if (rs != ALP_OK) cc3501e_dbg_fail_step = 6; /* bench debug (REVERT) */
	return rs;
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
#define CC3501E_SCAN_REC_HDR 10u

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
		uint8_t        ssid_len = rec[9];
		if (off + CC3501E_SCAN_REC_HDR + (size_t)ssid_len > got) {
			break; /* truncated trailing record -- stop cleanly */
		}
		cc3501e_scan_record_t *out = &out_records[n];
		memcpy(out->bssid, &rec[0], 6);
		out->rssi_dbm = (int8_t)rec[6];
		out->channel  = rec[7];
		out->security = rec[8];
		out->ssid_len = ssid_len;
		uint8_t copy  = (ssid_len > CC3501E_SSID_MAX) ? (uint8_t)CC3501E_SSID_MAX : ssid_len;
		memcpy(out->ssid, &rec[CC3501E_SCAN_REC_HDR], copy);
		out->ssid[copy] = '\0';
		off += CC3501E_SCAN_REC_HDR + (size_t)ssid_len;
		n++;
	}
	if (count != NULL) *count = n;
	return ALP_OK;
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
	memcpy(ip, reply, 4);
	return ALP_OK;
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
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_GPIO_CONFIGURE, (const uint8_t *)&c, sizeof(c), NULL, 0, NULL, timeout_ms);
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

alp_status_t cc3501e_gpio_set_interrupt(cc3501e_t              *ctx,
                                        uint8_t                 pad,
                                        alp_cc3501e_gpio_edge_t edge,
                                        bool                    enabled,
                                        uint32_t                timeout_ms)
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

alp_status_t cc3501e_set_event_callback(cc3501e_t *ctx, cc3501e_event_cb_t cb, void *user)
{
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	ctx->event_cb   = cb;
	ctx->event_user = user;
	return ALP_OK;
}

void cc3501e_deinit(cc3501e_t *ctx)
{
	if (ctx == NULL) return;
	ctx->initialised = false;
	ctx->bus         = NULL;
}
