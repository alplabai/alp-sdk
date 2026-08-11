/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * CC3501E Wi-Fi host helpers (opcodes 0x03, 0x10..0x17, 0x1B).  See
 * <alp/chips/cc3501e/wifi.h> for the public API and
 * <alp/protocol/cc3501e.h> for the wire protocol.
 *
 * Each is a thin wrapper over cc3501e_request matching the opcodes +
 * payloads in <alp/protocol/cc3501e.h>.  WIRE GAPS (the protocol
 * header, owned by the firmware-side agent, has opcodes but NO reply
 * payload structs for these -- noted per-helper):
 *   - GET_MAC (0x03): reply data assumed to be the 6 MAC bytes.
 *   - WIFI_GET_RSSI (0x16): reply data assumed to be one int8 dBm.
 *   - WIFI_GET_IP (0x17): reply data assumed to be 4 IPv4 octets.
 *   - WIFI_SCAN_START (0x10): the header defines alp_cc3501e_scan_
 *     result_t and documents scan results as ASYNC events
 *     (EVT_WIFI_SCAN_RESULT 0x18) -- there is no synchronous
 *     count/list envelope.  This rev has no async-event line, so the
 *     helper assumes the firmware returns the packed records as the
 *     SCAN_START reply payload (each fixed 10-byte header + inline
 *     ssid_len SSID bytes).
 */

#include <string.h>
#include <stdint.h>

#include "cc3501e_internal.h"

/* Minimum budget for an op that can overlap a radio bring-up.  On this
 * no-host-IRQ rev the bridge link is DOWN while the CC35 runs a radio op
 * (Wlan_Start/RoleUp can take SECONDS); requests during that window read back
 * IO and must keep retrying.  Floor the get-MAC poll budget here so a small
 * caller timeout can't give up inside the down-window before the radio is up. */
#define CC3501E_WIFI_DOWN_WINDOW_MS 10000u

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
	if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
	if (out_records == NULL && cap > 0u) return ALP_ERR_INVAL;
	if (count != NULL) *count = 0;

	/* Serialize same-context reentrancy explicitly (issue #740): a caller
	 * that re-enters this same ctx (e.g. from inside a callback invoked
	 * further down the same call stack) while a scan is already decoding
	 * here would otherwise race on wifi_scan_buf below.  Report BUSY rather
	 * than alias.  NOTE this is a plain test-then-set bool, not an
	 * atomic/CAS: it catches same-call-stack reentrancy but does not by
	 * itself serialize two truly concurrent callers on the SAME ctx from
	 * separate threads/cores -- see the concurrency warning on
	 * cc3501e_poll_events() in <alp/chips/cc3501e/events.h>. */
	if (ctx->wifi_scan_busy) return ALP_ERR_BUSY;
	ctx->wifi_scan_busy = true;

	/* The scan records can fill the reply payload; receive into the
	 * context's own scratch buffer (per-instance -- see cc3501e_t's
	 * wifi_scan_buf comment; keeps cc3501e_request's rx_scratch free for
	 * the framing, and no longer aliases across cc3501e_t instances). */
	uint8_t     *scan_buf = ctx->wifi_scan_buf;
	size_t       got      = 0;
	alp_status_t s        = poll_by_repeat(ctx,
	                                       ALP_CC3501E_CMD_WIFI_SCAN_START,
	                                       NULL,
	                                       0,
	                                       scan_buf,
	                                       sizeof(ctx->wifi_scan_buf),
	                                       &got,
	                                       timeout_ms);
	if (s != ALP_OK) {
		ctx->wifi_scan_busy = false;
		return s;
	}

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
	ctx->wifi_scan_busy = false;
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

/* Backoff between CMD_WIFI_STATUS collect polls while cc3501e_wifi_connect()
 * awaits the outcome -- same cadence as poll_by_repeat's CC3501E_POLL_GAP_MS
 * (cc3501e_core.c), duplicated here rather than exported because this loop
 * polls a DIFFERENT opcode (WIFI_STATUS) than the one it is bounding
 * (WIFI_CONNECT_STA), so it cannot just call poll_by_repeat itself. */
#define CC3501E_WIFI_STATUS_POLL_GAP_MS 50u

/* Single, non-retried WIFI_STATUS read (opcode 0x1B) -- decodes the same
 * fixed 4-byte wire layout as the public cc3501e_wifi_status() below, but
 * WITHOUT its down-window poll_by_repeat.
 *
 * cc3501e_wifi_connect()'s own loop below already re-polls on its own
 * cadence (CC3501E_WIFI_STATUS_POLL_GAP_MS) for the caller's whole
 * timeout_ms budget.  Calling the public cc3501e_wifi_status() from inside
 * that loop layered ITS OWN internal retry (up to CC3501E_WIFI_DOWN_WINDOW_MS
 * = 10 s) underneath the outer loop, and the outer loop's
 * `remaining -= gap` accounting only ever saw the 50 ms gap it slept for --
 * never the up-to-10-s the inner call could burn on a wedged transport.
 * Measured against a wedged transport in this repo's own harness:
 * connect(timeout_ms=200) made 1005 WIFI_STATUS attempts = 50250 ms (251x
 * the declared budget) before giving up.
 *
 * A bounded single attempt here (CC3501E_REQ_TMO_MS, the same per-attempt
 * budget every other single-shot request in this file uses) keeps the outer
 * loop the SOLE owner of the retry budget, matching
 * <alp/chips/cc3501e/wifi.h>'s documented contract for cc3501e_wifi_connect
 * ("Upper bound on the WIFI_STATUS poll budget"). */
static alp_status_t wifi_status_once(cc3501e_t *ctx, alp_cc3501e_wifi_status_t *out)
{
	uint8_t      reply[4] = { 0 };
	size_t       got      = 0;
	alp_status_t s        = cc3501e_request(
	    ctx, ALP_CC3501E_CMD_WIFI_STATUS, NULL, 0, reply, sizeof(reply), &got, CC3501E_REQ_TMO_MS);
	if (s != ALP_OK) return s;
	if (got < sizeof(reply)) return ALP_ERR_IO; /* short reply -- firmware/wire gap */
	out->state       = reply[0];
	out->fail_reason = reply[1];
	out->rssi_dbm    = (int8_t)reply[2];
	out->reserved    = reply[3];
	return ALP_OK;
}

alp_status_t cc3501e_wifi_connect(cc3501e_t  *ctx,
                                  const char *ssid,
                                  uint8_t     sec_type,
                                  const char *pass,
                                  uint32_t    timeout_ms)
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

	/* SUBMIT ONCE (issues #1376/#1378) -- CONNECT_STA is fire-and-forget on
	 * the firmware side: WORKER_IDLE queues the association and acks
	 * RESP_ERR_BUSY, the drain runs the seconds-long body off THIS exchange,
	 * and the outcome is mirrored into the non-blocking WIFI_STATUS latch --
	 * the host never collects DONE/ERR through this job slot (see
	 * firmware/cc3501e/src/worker.c's worker_run_pending() comment).  The old
	 * contract poll_by_repeat()'d this opcode on the wire-level BUSY/IO
	 * retry rule; every 50 ms retry that landed on the now-IDLE slot (freed
	 * the instant the drain finished, BEFORE the READY re-arm) submitted a
	 * BRAND NEW association -- five retries, five real join attempts, for
	 * one user command.  A single request removes that loop entirely.
	 *
	 * The submit's own ack is NOT trusted as the connect result, in either
	 * direction.  Only ALP_ERR_INVAL short-circuits: wifi_join() on the
	 * firmware side validates the payload BEFORE touching the worker at all
	 * (protocol_wifi.c), so RESP_ERR_INVALID is a definite synchronous
	 * reject -- nothing was queued, there is nothing to await.  Every other
	 * outcome falls through to the INDEPENDENT WIFI_STATUS channel below,
	 * DELIBERATELY including ALP_ERR_IO: a transport-level fault on the
	 * submit exchange is itself ambiguous evidence (see cc3501e_core.c's
	 * dead-phase note) -- it may mean nothing reached the firmware, or it
	 * may mean a genuine BUSY ack was lost in transit after the submit
	 * landed.  Bailing out on that ambiguity would risk reporting a false
	 * failure for an association that is actually running; polling
	 * WIFI_STATUS instead is safe either way -- it reads DISCONNECTED
	 * (nothing running) through to a timeout if nothing was submitted, or
	 * tracks the real CONNECTING/CONNECTED/FAILED transition if it was.
	 * This is what closes #1378 for this specific race: even a spurious OK
	 * or a spurious IO on the submit exchange cannot make this function
	 * report ALP_OK for an association WIFI_STATUS never confirms. */
	alp_status_t s = cc3501e_request(
	    ctx, ALP_CC3501E_CMD_WIFI_CONNECT_STA, payload, off, NULL, 0, NULL, CC3501E_REQ_TMO_MS);
	if (s == ALP_ERR_INVAL) {
		return s; /* definite synchronous reject -- nothing submitted */
	}

	/* Await the outcome off the non-blocking WIFI_STATUS latch (opcode
	 * 0x1B) -- never off the submit's own ack.  CONNECTING means the
	 * association is still running; CONNECTED / CONN_FAILED are terminal.
	 * Uses wifi_status_once() (a single non-retried attempt), NOT the public
	 * cc3501e_wifi_status() -- see wifi_status_once()'s comment for why
	 * layering that function's own 10 s down-window retry underneath this
	 * loop broke the timeout_ms accounting. */
	uint32_t remaining = (timeout_ms > 0u) ? timeout_ms : 1u;
	for (;;) {
		alp_cc3501e_wifi_status_t st;
		alp_status_t              ss = wifi_status_once(ctx, &st);
		if (ss == ALP_OK) {
			if (st.state == ALP_CC3501E_WIFI_CONNECTED) return ALP_OK;
			if (st.state == ALP_CC3501E_WIFI_CONN_FAILED) {
				return (st.fail_reason == ALP_CC3501E_WIFI_FAIL_TIMEOUT) ? ALP_ERR_TIMEOUT
				                                                         : ALP_ERR_IO;
			}
			/* DISCONNECTED (not yet latched) or CONNECTING: keep polling. */
		}
		/* ss != ALP_OK: a single status read failing (e.g. a transient
		 * down-window IO) is worth one more pass rather than an immediate
		 * bail -- the next iteration will retry it.  Debit BOTH this
		 * attempt's own worst-case cost (CC3501E_REQ_TMO_MS -- the budget
		 * wifi_status_once() bounds a single request to) and the poll gap
		 * from the caller's declared timeout_ms: the attempt is no longer
		 * hidden inside an inner retry loop, so it must be charged here for
		 * `remaining` to stay an honest upper bound on wall-clock time. */
		uint32_t attempt_cost = (CC3501E_REQ_TMO_MS < remaining) ? CC3501E_REQ_TMO_MS : remaining;
		remaining -= attempt_cost;
		if (remaining == 0u) return ALP_ERR_TIMEOUT;
		uint32_t gap = (remaining < CC3501E_WIFI_STATUS_POLL_GAP_MS)
		                   ? remaining
		                   : CC3501E_WIFI_STATUS_POLL_GAP_MS;
		alp_delay_ms(gap);
		remaining -= gap;
	}
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

alp_status_t cc3501e_wifi_ap_start(cc3501e_t  *ctx,
                                   const char *ssid,
                                   uint8_t     sec_type,
                                   const char *pass,
                                   uint32_t    timeout_ms)
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
	uint8_t reply[1] = { 0 };
	size_t  got      = 0;
	/* WIFI_GET_RSSI is WORKER-ROUTED on the firmware side (handle_worker_
	 * routed in protocol_wifi.c: Wlan_Get(WLAN_GET_RSSI) lazy-starts the
	 * radio on first use, which can take seconds and so must run off the SPI
	 * ISR) -- the firmware acks a fresh submit with RESP_ERR_BUSY and only
	 * returns RESP_OK once the drain has collected the value.  #1377: a
	 * single cc3501e_request() here essentially never lands on the DONE
	 * poll -- it collects the submit's own BUSY ack, reports it up as
	 * ALP_ERR_BUSY, and leaves the job orphaned in the single job slot for
	 * the next unrelated worker-routed call to trip over.  Poll-by-repeat,
	 * floored to the radio down-window, exactly like cc3501e_wifi_get_mac. */
	alp_status_t s = poll_by_repeat(ctx,
	                                ALP_CC3501E_CMD_WIFI_GET_RSSI,
	                                NULL,
	                                0,
	                                reply,
	                                sizeof(reply),
	                                &got,
	                                CC3501E_WIFI_DOWN_WINDOW_MS);
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
	 * padding): state | fail_reason | rssi_dbm | reserved.  The FIRMWARE-side
	 * handler (handle_wifi_status) is a genuine non-blocking latch read -- no
	 * radio op, ISR-safe, always replies RESP_OK -- but the shared bridge
	 * TRANSPORT is briefly down whenever ANY radio op is in flight (Wlan_Start
	 * at boot, or a worker connect/scan/rssi body), exactly like every other
	 * op in this file.  #1377: a status read taken moments after a connect
	 * submit can land in that down-window and desync like any other
	 * transaction (ver / scan / connect all healthy, then every SUBSEQUENT
	 * status call failed -5) -- that is the shared TRANSPORT contract, not
	 * something specific to the WIFI_STATUS opcode, so this needs the same
	 * poll-by-repeat down-window handling as the radio-adjacent ops even
	 * though the opcode itself never replies BUSY.  Only the IO-retry leg of
	 * poll_by_repeat ever fires here in practice. */
	uint8_t      reply[4] = { 0 };
	size_t       got      = 0;
	alp_status_t s        = poll_by_repeat(ctx,
	                                       ALP_CC3501E_CMD_WIFI_STATUS,
	                                       NULL,
	                                       0,
	                                       reply,
	                                       sizeof(reply),
	                                       &got,
	                                       CC3501E_WIFI_DOWN_WINDOW_MS);
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
