/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: Wi-Fi command-family handlers (0x10..0x1F).
 * Split out of protocol.c (issue #461); protocol_dispatch() in
 * protocol.c still owns the single command-family switch that routes
 * here.
 */

#include <string.h>

#include "protocol_internal.h"

/* WIFI_SCAN_START (0x10): reply data = the packed AP-record list (each
 * record = bssid[6] | rssi(1) | channel(1) | security(1) | ssid_len(1)
 * then ssid_len SSID bytes -- the cc3501e_wifi_scan host parser's wire
 * format).  Worker-routed: the real Wlan_Scan + event rendezvous blocks
 * for seconds and MUST run off the SPI ISR (see handle_worker_routed). */
alp_cc3501e_resp_t handle_wifi_scan_start(const uint8_t *req,
                                          size_t         req_len,
                                          uint8_t       *reply_data,
                                          size_t         reply_cap,
                                          size_t        *reply_data_len)
{
	(void)req;
	return handle_worker_routed(
	    ALP_CC3501E_CMD_WIFI_SCAN_START, 0u, req_len, reply_data, reply_cap, reply_data_len);
}

alp_cc3501e_resp_t handle_wifi_scan_stop(const uint8_t *req,
                                         size_t         req_len,
                                         uint8_t       *reply_data,
                                         size_t         reply_cap,
                                         size_t        *reply_data_len)
{
	(void)req;
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_wifi_scan_stop());
}

/* WIFI_CONNECT_STA (0x12) / WIFI_AP_START (0x14): payload is an
 * alp_cc3501e_wifi_connect_t header followed by the inline ssid[ssid_len]
 * then psk[psk_len].  Validates the cumulative length exactly. */
/* Validate the connect/AP wire payload, then WORKER-ROUTE the association (it
 * blocks for seconds on the connect/IP event and so MUST NOT run in
 * protocol_dispatch's SPI-ISR context).  The raw req payload is forwarded to
 * the worker, which re-derives ssid/psk from the same struct in the drain. */
static alp_cc3501e_resp_t
wifi_join(const uint8_t *req, size_t req_len, int ap, size_t *reply_data_len)
{
	if (req_len < sizeof(alp_cc3501e_wifi_connect_t)) return ALP_CC3501E_RESP_ERR_INVALID;
	const alp_cc3501e_wifi_connect_t *c = (const alp_cc3501e_wifi_connect_t *)req;
	const size_t                      need =
	    sizeof(alp_cc3501e_wifi_connect_t) + (size_t)c->ssid_len + (size_t)c->psk_len;
	if (req_len != need) return ALP_CC3501E_RESP_ERR_INVALID;
	const alp_cc3501e_cmd_t cmd =
	    ap ? ALP_CC3501E_CMD_WIFI_AP_START : ALP_CC3501E_CMD_WIFI_CONNECT_STA;
	return handle_worker_routed_payload(cmd, req, req_len, reply_data_len);
}

alp_cc3501e_resp_t handle_wifi_connect_sta(const uint8_t *req,
                                           size_t         req_len,
                                           uint8_t       *reply_data,
                                           size_t         reply_cap,
                                           size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	return wifi_join(req, req_len, 0, reply_data_len);
}

alp_cc3501e_resp_t handle_wifi_ap_start(const uint8_t *req,
                                        size_t         req_len,
                                        uint8_t       *reply_data,
                                        size_t         reply_cap,
                                        size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	return wifi_join(req, req_len, 1, reply_data_len);
}

alp_cc3501e_resp_t handle_wifi_disconnect(const uint8_t *req,
                                          size_t         req_len,
                                          uint8_t       *reply_data,
                                          size_t         reply_cap,
                                          size_t        *reply_data_len)
{
	(void)req;
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_wifi_disconnect());
}

alp_cc3501e_resp_t handle_wifi_ap_stop(const uint8_t *req,
                                       size_t         req_len,
                                       uint8_t       *reply_data,
                                       size_t         reply_cap,
                                       size_t        *reply_data_len)
{
	(void)req;
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_wifi_ap_stop());
}

/* WIFI_GET_RSSI (0x16): reply data = signed RSSI in dBm (1 byte).
 * Worker-routed: the real Wlan_Get(WLAN_GET_RSSI) lazy-starts the radio on
 * first use (seconds) and so MUST run off the SPI ISR (see
 * handle_worker_routed). */
alp_cc3501e_resp_t handle_wifi_get_rssi(const uint8_t *req,
                                        size_t         req_len,
                                        uint8_t       *reply_data,
                                        size_t         reply_cap,
                                        size_t        *reply_data_len)
{
	(void)req;
	return handle_worker_routed(
	    ALP_CC3501E_CMD_WIFI_GET_RSSI, 1u, req_len, reply_data, reply_cap, reply_data_len);
}

/* WIFI_GET_IP (0x17): reply data = 4-byte IPv4 address. */
alp_cc3501e_resp_t handle_wifi_get_ip(const uint8_t *req,
                                      size_t         req_len,
                                      uint8_t       *reply_data,
                                      size_t         reply_cap,
                                      size_t        *reply_data_len)
{
	(void)req;
	*reply_data_len = 0u;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	if (reply_cap < 4u) return ALP_CC3501E_RESP_ERR_NO_MEM;
	uint8_t            ip[4] = { 0 };
	alp_cc3501e_resp_t st    = hw_to_resp(cc3501e_hw_wifi_get_ip(ip));
	if (st == ALP_CC3501E_RESP_OK) {
		memcpy(reply_data, ip, 4u);
		*reply_data_len = 4u;
	}
	return st;
}

/* WIFI_STATUS (0x1B): reply data = alp_cc3501e_wifi_status_t
 * { state(1) | fail_reason(1) | rssi_dbm(int8) | reserved(1) }.  A NON-BLOCKING
 * read of the firmware connection-status latch (no radio op -- safe in the SPI
 * ISR), so the host can collect an async connect outcome without poll-by-repeat
 * on WIFI_CONNECT_STA (which clocked the bridge while the radio op held it down). */
alp_cc3501e_resp_t handle_wifi_status(const uint8_t *req,
                                      size_t         req_len,
                                      uint8_t       *reply_data,
                                      size_t         reply_cap,
                                      size_t        *reply_data_len)
{
	(void)req;
	*reply_data_len = 0u;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	if (reply_cap < 4u) return ALP_CC3501E_RESP_ERR_NO_MEM;
	uint8_t state = 0u, fail_reason = 0u;
	int8_t  rssi = 0;
	(void)cc3501e_hw_wifi_conn_status(&state, &fail_reason, &rssi);
	reply_data[0]   = state;
	reply_data[1]   = fail_reason;
	reply_data[2]   = (uint8_t)rssi;
	reply_data[3]   = 0u;
	*reply_data_len = 4u;
	return ALP_CC3501E_RESP_OK;
}
