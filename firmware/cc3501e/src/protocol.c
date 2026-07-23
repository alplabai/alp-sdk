/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: top-level dispatch + transport-agnostic
 * framing.
 *
 * Both the SPI transport (default) and the SDIO transport (optional,
 * customer-selectable) feed validated request frames into
 * protocol_dispatch() here -- one command set, one set of reply codes,
 * transport-agnostic (the gd32-bridge SPI + I2C model).
 *
 * The per-command-family handlers (META, stream-write, GPIO proxy,
 * camera enables, Wi-Fi, sockets, BLE, power policy, diagnostics, OTA)
 * live in the sibling protocol_<family>.c TUs (split out of this file,
 * issue #461); this file keeps only:
 *
 *   - protocol_dispatch(): the single opcode -> handler switch (the
 *     command-family "table" the header/README describe).
 *   - protocol_build_reply(): the shared request/reply framing wrapper
 *     every transport calls.
 *   - the worker-routed state-machine helpers (handle_worker_routed and
 *     friends) that several families' handlers call to move a blocking
 *     HAL body off the SPI ISR -- declared in protocol_internal.h,
 *     defined here as the one shared TU.
 *   - the diagnostics frame counters (g_last_error / g_frames_ok /
 *     g_frames_err) protocol_build_reply() updates centrally; the
 *     GET_DIAG_INFO / DIAG_GET_STATS handlers in protocol_diag.c read
 *     them via protocol_internal.h's extern declarations.
 *
 * v0.1 ("bring-up") scope: the META group only (PING, GET_VERSION,
 * GET_MAC, RESET).  Every other opcode -- Wi-Fi, BLE, GPIO proxy,
 * power, diagnostics -- is rejected with ALP_CC3501E_RESP_ERR_INVALID
 * per the protocol header's contract ("v1 firmware rejects opcodes it
 * does not implement with ALP_CC3501E_RESP_ERR_INVALID").  Those land
 * in v0.2+ and route to TI's SimpleLink Wi-Fi / BLE APIs through the
 * HAL backend; see docs/cc3501e-bridge.md "v0.x roadmap".
 */

#include "protocol_internal.h"

/* Diagnostics state (firmware-side): last_error = the most recent non-OK
 * response emitted; the frame counters feed DIAG_GET_STATS.  All are
 * updated centrally here in protocol_build_reply(); protocol_diag.c only
 * reads them (see the extern declarations in protocol_internal.h). */
uint8_t  g_last_error = ALP_CC3501E_RESP_OK;
uint32_t g_frames_ok;
uint32_t g_frames_err;

/* --------------------------------------------------------------- */
/* Worker-routed state-machine helpers                               */
/* --------------------------------------------------------------- */

/* Generic worker-routed handler for an ARGUMENT-FREE command whose HAL body
 * may BLOCK for seconds (a radio op) and therefore must NOT run in the SPI
 * ISR that dispatches the handler.  GET_MAC pioneered the seam (P0-4/P0-6);
 * WIFI_SCAN_START and WIFI_GET_RSSI share it -- the only per-command knobs
 * are the opcode and the reply-capacity floor.
 *
 * The command is POLL-BY-REPEAT: the host re-issues it (it maps
 * RESP_ERR_BUSY -> ALP_ERR_BUSY and retries) until the worker has the
 * result.  The state machine, identical to the original handle_get_mac:
 *
 *   - worker has DONE for @cmd -> copy the result bytes into the reply,
 *     reset the worker to IDLE, return RESP_OK.
 *   - worker has ERR for @cmd  -> reset, map the HAL code (NOTIMPL ->
 *     NOT_READY, INVAL -> INVALID, STATE -> the distinct ERR_STATE -- see
 *     <alp/protocol/cc3501e.h> -- else RADIO).  The stub backend lands
 *     here (NOT_READY).
 *   - worker IDLE              -> submit the job, return BUSY.
 *   - worker QUEUED/RUNNING, or busy with another cmd -> return BUSY.
 *
 * @min_cap is the reply-data floor (NO_MEM below it -- the worker's DONE
 * copy is capped at @reply_cap, so a too-small reply buffer must be caught
 * up front).  @req_len must be 0 (the routed ops carry no request payload).
 *
 * On the stub/native backend worker_submit() runs the job synchronously,
 * so the host needs exactly one extra poll (submit -> BUSY, re-issue ->
 * the cached NOT_READY/RESP_OK). */
alp_cc3501e_resp_t handle_worker_routed(alp_cc3501e_cmd_t cmd,
                                        size_t            min_cap,
                                        size_t            req_len,
                                        uint8_t          *reply_data,
                                        size_t            reply_cap,
                                        size_t           *reply_data_len)
{
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	if (reply_cap < min_cap) return ALP_CC3501E_RESP_ERR_NO_MEM;

	size_t                  n   = 0u;
	int8_t                  err = 0;
	const enum worker_state st  = worker_poll((uint8_t)cmd, reply_data, reply_cap, &n, &err);

	switch (st) {
	case WORKER_DONE:
		worker_reset();
		*reply_data_len = n; /* command-specific payload (6 for GET_MAC, etc.) */
		return ALP_CC3501E_RESP_OK;
	case WORKER_ERR:
		worker_reset();
		if (err == CC3501E_HW_ERR_NOTIMPL) return ALP_CC3501E_RESP_ERR_NOT_READY;
		if (err == CC3501E_HW_ERR_INVAL) return ALP_CC3501E_RESP_ERR_INVALID;
		/* CC3501E_HW_ERR_STATE: today only cc3501e_hw_ble_gatt_register's NimBLE
		 * ble_gatts_mutable() reject (BLE_HS_EBUSY) -- a deterministic, terminal
		 * refusal, not the generic radio/protocol RESP_ERR_RADIO bucket below. */
		if (err == CC3501E_HW_ERR_STATE) return ALP_CC3501E_RESP_ERR_STATE;
		return ALP_CC3501E_RESP_ERR_RADIO;
	case WORKER_IDLE:
		/* No job in flight: queue one and ask the host to re-issue. */
		(void)worker_submit((uint8_t)cmd);
		return ALP_CC3501E_RESP_ERR_BUSY;
	default: /* WORKER_QUEUED / WORKER_RUNNING (incl. another cmd in flight) */
		return ALP_CC3501E_RESP_ERR_BUSY;
	}
}

/* As handle_worker_routed, but for a job that CARRIES a request payload
 * (WIFI_CONNECT_STA / WIFI_AP_START).  The caller must have validated @p req /
 * @p req_len already; on the IDLE edge the payload is stashed via
 * worker_submit_payload so the drain runs the blocking association off the SPI
 * ISR.  These ops carry no reply payload (min_cap 0). */
alp_cc3501e_resp_t handle_worker_routed_payload(alp_cc3501e_cmd_t cmd,
                                                const uint8_t    *req,
                                                size_t            req_len,
                                                size_t           *reply_data_len)
{
	*reply_data_len = 0u;

	size_t                  n   = 0u;
	int8_t                  err = 0;
	const enum worker_state st  = worker_poll((uint8_t)cmd, NULL, 0u, &n, &err);

	switch (st) {
	case WORKER_DONE:
		worker_reset();
		return ALP_CC3501E_RESP_OK;
	case WORKER_ERR:
		worker_reset();
		if (err == CC3501E_HW_ERR_NOTIMPL) return ALP_CC3501E_RESP_ERR_NOT_READY;
		if (err == CC3501E_HW_ERR_INVAL) return ALP_CC3501E_RESP_ERR_INVALID;
		/* CC3501E_HW_ERR_STATE: today only cc3501e_hw_ble_gatt_register's NimBLE
		 * ble_gatts_mutable() reject (BLE_HS_EBUSY) -- a deterministic, terminal
		 * refusal, not the generic radio/protocol RESP_ERR_RADIO bucket below. */
		if (err == CC3501E_HW_ERR_STATE) return ALP_CC3501E_RESP_ERR_STATE;
		return ALP_CC3501E_RESP_ERR_RADIO;
	case WORKER_IDLE:
		/* No job in flight: queue THIS one (with its payload) + return BUSY (the
		 * submit ack).  For a STA connect, latch the status to CONNECTING NOW --
		 * synchronously, before the drain runs the seconds-long body -- so a host
		 * CMD_WIFI_STATUS poll during the brief queued window never reads a stale
		 * CONNECTED/FAILED from a previous attempt.  The body then publishes the
		 * terminal CONNECTED/FAILED; the host collects it via CMD_WIFI_STATUS (no
		 * poll-by-repeat on the connect opcode). */
		if (cmd == ALP_CC3501E_CMD_WIFI_CONNECT_STA) {
			cc3501e_hw_wifi_mark_connecting();
		}
		(void)worker_submit_payload((uint8_t)cmd, req, (uint16_t)req_len);
		return ALP_CC3501E_RESP_ERR_BUSY;
	default: /* QUEUED / RUNNING (incl. another cmd in flight) */
		return ALP_CC3501E_RESP_ERR_BUSY;
	}
}

/* As handle_worker_routed_payload, but for a payload-carrying job that ALSO
 * returns reply DATA (BLE_GATT_READ: the attribute handle goes up in the
 * payload, the attribute value comes back).  The worker copies its DONE bytes
 * straight into @p reply_data -- the SAME reply path handle_worker_routed uses
 * for the record-returning scans -- so a single job round-trips a value off the
 * SPI ISR.  Poll-by-repeat, @min_cap is the reply-data floor.  The caller must
 * have validated @p req / @p req_len already. */
alp_cc3501e_resp_t handle_worker_routed_payload_reply(alp_cc3501e_cmd_t cmd,
                                                      const uint8_t    *req,
                                                      size_t            req_len,
                                                      size_t            min_cap,
                                                      uint8_t          *reply_data,
                                                      size_t            reply_cap,
                                                      size_t           *reply_data_len)
{
	*reply_data_len = 0u;
	if (reply_cap < min_cap) return ALP_CC3501E_RESP_ERR_NO_MEM;

	size_t                  n   = 0u;
	int8_t                  err = 0;
	const enum worker_state st  = worker_poll((uint8_t)cmd, reply_data, reply_cap, &n, &err);

	switch (st) {
	case WORKER_DONE:
		worker_reset();
		*reply_data_len = n; /* the worker copied the attribute value into reply_data */
		return ALP_CC3501E_RESP_OK;
	case WORKER_ERR:
		worker_reset();
		if (err == CC3501E_HW_ERR_NOTIMPL) return ALP_CC3501E_RESP_ERR_NOT_READY;
		if (err == CC3501E_HW_ERR_INVAL) return ALP_CC3501E_RESP_ERR_INVALID;
		/* CC3501E_HW_ERR_STATE: today only cc3501e_hw_ble_gatt_register's NimBLE
		 * ble_gatts_mutable() reject (BLE_HS_EBUSY) -- a deterministic, terminal
		 * refusal, not the generic radio/protocol RESP_ERR_RADIO bucket below. */
		if (err == CC3501E_HW_ERR_STATE) return ALP_CC3501E_RESP_ERR_STATE;
		return ALP_CC3501E_RESP_ERR_RADIO;
	case WORKER_IDLE:
		/* No job in flight: queue THIS one (with its payload) + return BUSY. */
		(void)worker_submit_payload((uint8_t)cmd, req, (uint16_t)req_len);
		return ALP_CC3501E_RESP_ERR_BUSY;
	default: /* QUEUED / RUNNING (incl. another cmd in flight) */
		return ALP_CC3501E_RESP_ERR_BUSY;
	}
}

/* --------------------------------------------------------------- */
/* Dispatch                                                          */
/* --------------------------------------------------------------- */

typedef alp_cc3501e_resp_t (*cmd_handler_t)(const uint8_t *, size_t, uint8_t *, size_t, size_t *);

/* Sparse switch on opcode -- keeps the table small without losing the
 * single-handler-table property.  v0.2+ feature groups slot in here as
 * their HAL bodies land.  Each case routes to the owning family's
 * protocol_<family>.c handler (declared in protocol_internal.h). */
alp_cc3501e_resp_t protocol_dispatch(uint8_t        cmd,
                                     uint8_t        flags,
                                     const uint8_t *req,
                                     size_t         req_len,
                                     uint8_t       *reply_data,
                                     size_t         reply_cap,
                                     size_t        *reply_data_len)
{
	(void)flags; /* v0.1 has no request flag that alters dispatch. */
	*reply_data_len = 0u;

	cmd_handler_t h = NULL;
	switch (cmd) {
	case ALP_CC3501E_CMD_PING:
		h = handle_ping;
		break;
	case ALP_CC3501E_CMD_GET_VERSION:
		h = handle_get_version;
		break;
	case ALP_CC3501E_CMD_GET_MAC:
		h = handle_get_mac;
		break;
	case ALP_CC3501E_CMD_RESET:
		h = handle_reset;
		break;
	case ALP_CC3501E_CMD_GET_DIAG_INFO:
		h = handle_get_diag_info;
		break;
	case ALP_CC3501E_CMD_GET_PENDING_EVENTS:
		h = handle_get_pending_events;
		break;
	/* OTA firmware update (v0.2). */
	case ALP_CC3501E_CMD_OTA_BEGIN:
		h = handle_ota_begin;
		break;
	case ALP_CC3501E_CMD_OTA_WRITE:
		h = handle_ota_write;
		break;
	case ALP_CC3501E_CMD_OTA_FINISH:
		h = handle_ota_finish;
		break;
	case ALP_CC3501E_CMD_OTA_ABORT:
		h = handle_ota_abort;
		break;
	case ALP_CC3501E_CMD_OTA_STATUS:
		h = handle_ota_status;
		break;
	case ALP_CC3501E_CMD_OTA_PROMOTE:
		h = handle_ota_promote;
		break;
	case ALP_CC3501E_CMD_STREAM_WRITE:
		h = handle_stream_write;
		break;
	/* GPIO proxy + camera enables (v0.4). */
	case ALP_CC3501E_CMD_GPIO_CONFIGURE:
		h = handle_gpio_configure;
		break;
	case ALP_CC3501E_CMD_GPIO_WRITE:
		h = handle_gpio_write;
		break;
	case ALP_CC3501E_CMD_GPIO_READ:
		h = handle_gpio_read;
		break;
	case ALP_CC3501E_CMD_GPIO_SET_INTERRUPT:
		h = handle_gpio_set_interrupt;
		break;
	case ALP_CC3501E_CMD_CAM_ENABLE:
		h = handle_cam_enable;
		break;
	case ALP_CC3501E_CMD_CAM_DISABLE:
		h = handle_cam_disable;
		break;
	/* Wi-Fi (v0.2). */
	case ALP_CC3501E_CMD_WIFI_SCAN_START:
		h = handle_wifi_scan_start;
		break;
	case ALP_CC3501E_CMD_WIFI_SCAN_STOP:
		h = handle_wifi_scan_stop;
		break;
	case ALP_CC3501E_CMD_WIFI_CONNECT_STA:
		h = handle_wifi_connect_sta;
		break;
	case ALP_CC3501E_CMD_WIFI_DISCONNECT:
		h = handle_wifi_disconnect;
		break;
	case ALP_CC3501E_CMD_WIFI_AP_START:
		h = handle_wifi_ap_start;
		break;
	case ALP_CC3501E_CMD_WIFI_AP_STOP:
		h = handle_wifi_ap_stop;
		break;
	case ALP_CC3501E_CMD_WIFI_GET_RSSI:
		h = handle_wifi_get_rssi;
		break;
	case ALP_CC3501E_CMD_WIFI_GET_IP:
		h = handle_wifi_get_ip;
		break;
	case ALP_CC3501E_CMD_WIFI_STATUS:
		h = handle_wifi_status;
		break;
	/* TCP/UDP sockets (v0.5). */
	case ALP_CC3501E_CMD_SOCK_OPEN:
		h = handle_sock_open;
		break;
	case ALP_CC3501E_CMD_SOCK_CONNECT:
		h = handle_sock_connect;
		break;
	case ALP_CC3501E_CMD_SOCK_SEND:
		h = handle_sock_send;
		break;
	case ALP_CC3501E_CMD_SOCK_RECV:
		h = handle_sock_recv;
		break;
	case ALP_CC3501E_CMD_SOCK_CLOSE:
		h = handle_sock_close;
		break;
	/* BLE 5.4 (v0.3). */
	case ALP_CC3501E_CMD_BLE_ENABLE:
		h = handle_ble_enable;
		break;
	case ALP_CC3501E_CMD_BLE_DISABLE:
		h = handle_ble_disable;
		break;
	case ALP_CC3501E_CMD_BLE_ADV_START:
		h = handle_ble_adv_start;
		break;
	case ALP_CC3501E_CMD_BLE_ADV_STOP:
		h = handle_ble_adv_stop;
		break;
	case ALP_CC3501E_CMD_BLE_SCAN_START:
		h = handle_ble_scan_start;
		break;
	case ALP_CC3501E_CMD_BLE_SCAN_STOP:
		h = handle_ble_scan_stop;
		break;
	case ALP_CC3501E_CMD_BLE_CONNECT:
		h = handle_ble_connect;
		break;
	case ALP_CC3501E_CMD_BLE_DISCONNECT:
		h = handle_ble_disconnect;
		break;
	case ALP_CC3501E_CMD_BLE_GATT_REGISTER:
		h = handle_ble_gatt_register;
		break;
	case ALP_CC3501E_CMD_BLE_GATT_NOTIFY:
		h = handle_ble_gatt_notify;
		break;
	case ALP_CC3501E_CMD_BLE_GATT_READ:
		h = handle_ble_gatt_read;
		break;
	case ALP_CC3501E_CMD_BLE_GATT_WRITE:
		h = handle_ble_gatt_write;
		break;
	/* Power policy + diagnostics (configurability). */
	case ALP_CC3501E_CMD_POWER_POLICY:
		h = handle_power_policy;
		break;
	case ALP_CC3501E_CMD_DIAG_GET_STATS:
		h = handle_diag_get_stats;
		break;
	case ALP_CC3501E_CMD_DIAG_LOG_LEVEL:
		h = handle_diag_log_level;
		break;
	default:
		/* Unknown, or a known v1 opcode whose firmware body has not
         * landed yet (all of Wi-Fi / BLE / GPIO / power / diag in
         * v0.1).  The header's contract is RESP_ERR_INVALID. */
		return ALP_CC3501E_RESP_ERR_INVALID;
	}
	return h(req, req_len, reply_data, reply_cap, reply_data_len);
}

/* --------------------------------------------------------------- */
/* Transport-agnostic framing                                        */
/* --------------------------------------------------------------- */

size_t protocol_build_reply(const uint8_t *req_frame,
                            size_t         req_len,
                            uint8_t       *reply_frame,
                            size_t         reply_cap)
{
	/* The caller guarantees a full-size reply buffer; guard anyway. */
	if (reply_cap < CC3501E_REPLY_DATA_OFF) {
		return 0u;
	}

	const uint8_t      cmd_echo = (req_len >= 1u) ? req_frame[0] : 0u;
	alp_cc3501e_resp_t status   = ALP_CC3501E_RESP_ERR_PROTOCOL;
	size_t             data_len = 0u;

	if (req_len >= (size_t)ALP_CC3501E_HEADER_BYTES) {
		const uint8_t  flags       = req_frame[1];
		const uint16_t payload_len = (uint16_t)req_frame[2] | ((uint16_t)req_frame[3] << 8);

		/* Captured byte count must match the declared payload exactly. */
		if ((size_t)ALP_CC3501E_HEADER_BYTES + (size_t)payload_len == req_len) {
			const uint8_t *req = (payload_len > 0u) ? &req_frame[ALP_CC3501E_HEADER_BYTES] : NULL;
			status             = protocol_dispatch(cmd_echo,
			                                       flags,
			                                       req,
			                                       payload_len,
			                                       &reply_frame[CC3501E_REPLY_DATA_OFF],
			                                       reply_cap - CC3501E_REPLY_DATA_OFF,
			                                       &data_len);
		}
	}

	/* Defence-in-depth: a handler must never report more data than the reply
	 * buffer holds, but if one did, (uint16_t)(1u + data_len) would TRUNCATE the
	 * length silently and frame a corrupt reply.  Clamp + fail closed instead. */
	const size_t reply_data_cap =
	    (reply_cap > CC3501E_REPLY_DATA_OFF) ? (reply_cap - CC3501E_REPLY_DATA_OFF) : 0u;
	if (data_len > reply_data_cap) {
		data_len = 0u;
		status   = ALP_CC3501E_RESP_ERR_NO_MEM;
	}

	/* Diagnostics bookkeeping: count OK vs error replies and latch the last
	 * non-OK status, for GET_DIAG_INFO / DIAG_GET_STATS.  Runs AFTER the clamp so
	 * a clamped NO_MEM is counted as the error it is. */
	if (status == ALP_CC3501E_RESP_OK) {
		g_frames_ok++;
	} else {
		g_frames_err++;
		g_last_error = (uint8_t)status;
	}

	/* Frame the reply: [cmd | flags=0 | payload_len(LE) | status | data].
     * flags = 0 -> solicited reply (async events set FLAG_ASYNC_EVENT in
     * v0.2+).  payload = status(1) + data. */
	const uint16_t reply_payload          = (uint16_t)(1u + data_len);
	reply_frame[0]                        = cmd_echo;
	reply_frame[1]                        = 0u;
	reply_frame[2]                        = (uint8_t)(reply_payload & 0xFFu);
	reply_frame[3]                        = (uint8_t)((reply_payload >> 8) & 0xFFu);
	reply_frame[CC3501E_REPLY_STATUS_OFF] = (uint8_t)status;
	return (size_t)ALP_CC3501E_HEADER_BYTES + reply_payload;
}
