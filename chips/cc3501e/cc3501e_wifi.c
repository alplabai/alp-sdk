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
	 * (or a caller re-entering from another thread/ISR) that starts a second
	 * scan on THIS ctx while one is already decoding here would otherwise
	 * race on wifi_scan_buf below.  Report BUSY rather than alias. */
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
