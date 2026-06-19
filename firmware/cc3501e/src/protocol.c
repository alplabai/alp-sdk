/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: shared command-handler table.
 *
 * Both the SPI transport (default) and the SDIO transport (optional,
 * customer-selectable) feed validated request frames into
 * protocol_dispatch() here -- one command set, one set of reply codes,
 * transport-agnostic (the gd32-bridge SPI + I2C model).
 *
 * v0.1 ("bring-up") scope: the META group only (PING, GET_VERSION,
 * GET_MAC, RESET).  Every other opcode -- Wi-Fi, BLE, GPIO proxy,
 * power, diagnostics -- is rejected with ALP_CC3501E_RESP_ERR_INVALID
 * per the protocol header's contract ("v1 firmware rejects opcodes it
 * does not implement with ALP_CC3501E_RESP_ERR_INVALID").  Those land
 * in v0.2+ and route to TI's SimpleLink Wi-Fi / BLE APIs through the
 * HAL backend; see docs/cc3501e-bridge.md "v0.x roadmap".
 *
 * Handlers that touch hardware (MAC read, self-reset) call the
 * cc3501e_hw_* shims declared in ../hal/cc3501e_hw.h.  The stub backend
 * (hal/cc3501e_hw_stub.c) keeps the protocol path exercisable on the
 * host with no TI SDK; the real bodies live in hal/ti/ and link against
 * TI's SimpleLink CC33xx SDK on the bench build.
 */

#include <string.h>

#include "protocol.h"
#include "worker.h"
#include "../hal/cc3501e_hw.h"

/* --------------------------------------------------------------- */
/* Little-endian helpers (firmware side, parallel to the host).      */
/* --------------------------------------------------------------- */

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
	p[2] = (uint8_t)((v >> 16) & 0xFFu);
	p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Firmware *release* version reported by GET_DIAG_INFO.fw_version (u16) --
 * distinct from ALP_CC3501E_PROTOCOL_VERSION (GET_VERSION).  Encodes
 * firmware-version.txt 0.1.0 as 0x0001 (the first release). */
#define CC3501E_BRIDGE_FW_VERSION_U16 0x0001u

/* Diagnostics state (firmware-side): last_error = the most recent non-OK
 * response emitted; the frame counters feed DIAG_GET_STATS.  All are
 * updated centrally in protocol_build_reply(). */
static uint8_t  g_last_error = ALP_CC3501E_RESP_OK;
static uint32_t g_frames_ok;
static uint32_t g_frames_err;

/* --------------------------------------------------------------- */
/* META handlers (opcodes 0x00..0x0F)                                */
/* --------------------------------------------------------------- */

/* PING (0x00): liveness probe.  Empty request, empty reply data --
 * the bare RESP_OK status the transport prepends is the "firmware is
 * alive" signal the host's first post-boot handshake waits for. */
static alp_cc3501e_resp_t handle_ping(const uint8_t *req,
                                      size_t         req_len,
                                      uint8_t       *reply_data,
                                      size_t         reply_cap,
                                      size_t        *reply_data_len)
{
	(void)req;
	(void)reply_data;
	(void)reply_cap;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	*reply_data_len = 0u;
	return ALP_CC3501E_RESP_OK;
}

/* GET_VERSION (0x01): wire-protocol compatibility gate.  Returns the
 * 16-bit ALP_CC3501E_PROTOCOL_VERSION (LE); the host refuses to talk
 * to a firmware whose value != its compile-time ALP_CC3501E_PROTOCOL_VERSION.
 *
 * This returns the PROTOCOL version, not the firmware RELEASE version
 * (firmware-version.txt), matching the host driver's compatibility
 * check `version != ALP_CC3501E_PROTOCOL_VERSION` and the chip-header
 * doc.  (The diag-struct comment in <alp/protocol/cc3501e.h> that says
 * GET_VERSION returns the release version is a documentation
 * discrepancy -- tracked in DESIGN.md; the release version is reported
 * separately via GET_DIAG_INFO.fw_version in v2 firmware.) */
static alp_cc3501e_resp_t handle_get_version(const uint8_t *req,
                                             size_t         req_len,
                                             uint8_t       *reply_data,
                                             size_t         reply_cap,
                                             size_t        *reply_data_len)
{
	(void)req;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	if (reply_cap < 2u) return ALP_CC3501E_RESP_ERR_NO_MEM;
	put_le16(reply_data, (uint16_t)ALP_CC3501E_PROTOCOL_VERSION);
	*reply_data_len = 2u;
	return ALP_CC3501E_RESP_OK;
}

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
 *     NOT_READY, INVAL -> INVALID, else RADIO).  The stub backend lands
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
static alp_cc3501e_resp_t handle_worker_routed(alp_cc3501e_cmd_t cmd,
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
		return ALP_CC3501E_RESP_ERR_RADIO;
	case WORKER_IDLE:
		/* No job in flight: queue one and ask the host to re-issue. */
		(void)worker_submit((uint8_t)cmd);
		return ALP_CC3501E_RESP_ERR_BUSY;
	default: /* WORKER_QUEUED / WORKER_RUNNING (incl. another cmd in flight) */
		return ALP_CC3501E_RESP_ERR_BUSY;
	}
}

/* GET_MAC (0x03): the CC3501E's factory MAC (6 bytes, big-endian wire
 * order as TI stores it).  Read from the radio subsystem via the HAL.
 *
 * Routed through the async worker (P0-4/P0-6): the real CC3501E_WIFI body
 * (Wlan_Get, preceded by a one-time Wi-Fi init) blocks for SECONDS, which
 * MUST NOT happen in the SPI ISR that runs this handler.  See
 * handle_worker_routed for the poll-by-repeat state machine. */
static alp_cc3501e_resp_t handle_get_mac(const uint8_t *req,
                                         size_t         req_len,
                                         uint8_t       *reply_data,
                                         size_t         reply_cap,
                                         size_t        *reply_data_len)
{
	(void)req;
	return handle_worker_routed(
	    ALP_CC3501E_CMD_GET_MAC, 6u, req_len, reply_data, reply_cap, reply_data_len);
}

/* RESET (0x02): host-requested soft reset.  Reply OK now; the HAL
 * defers the actual reboot until after the reply has been clocked back
 * to the host (so the host sees the ack), then resets the chip -- which
 * the host detects as the firmware going quiet then re-PINGing alive.
 * On the stub backend the deferred reset is a no-op. */
static alp_cc3501e_resp_t handle_reset(const uint8_t *req,
                                       size_t         req_len,
                                       uint8_t       *reply_data,
                                       size_t         reply_cap,
                                       size_t        *reply_data_len)
{
	(void)req;
	(void)reply_data;
	(void)reply_cap;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	cc3501e_hw_request_reset();
	*reply_data_len = 0u;
	return ALP_CC3501E_RESP_OK;
}

/* --------------------------------------------------------------- */
/* GPIO proxy (0x50..0x5F) + camera enables (0x60..0x61)             */
/* --------------------------------------------------------------- */

/* Map a HAL return code (CC3501E_HW_*) onto a wire response status.
 * NOTIMPL -> NOT_READY (the op is not wired on this backend yet);
 * INVAL -> INVALID; any other failure -> RADIO (generic HW/IO). */
static alp_cc3501e_resp_t hw_to_resp(int rv)
{
	switch (rv) {
	case CC3501E_HW_OK:
		return ALP_CC3501E_RESP_OK;
	case CC3501E_HW_ERR_NOTIMPL:
		return ALP_CC3501E_RESP_ERR_NOT_READY;
	case CC3501E_HW_ERR_INVAL:
		return ALP_CC3501E_RESP_ERR_INVALID;
	default:
		return ALP_CC3501E_RESP_ERR_RADIO;
	}
}

/* GPIO_CONFIGURE (0x50): set direction + pull on a proxied CC3501E pad. */
static alp_cc3501e_resp_t handle_gpio_configure(const uint8_t *req,
                                                size_t         req_len,
                                                uint8_t       *reply_data,
                                                size_t         reply_cap,
                                                size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != sizeof(alp_cc3501e_gpio_configure_t)) return ALP_CC3501E_RESP_ERR_INVALID;
	const alp_cc3501e_gpio_configure_t *c = (const alp_cc3501e_gpio_configure_t *)req;
	return hw_to_resp(cc3501e_hw_gpio_configure(c->cc3501e_gpio, c->direction, c->pull));
}

/* GPIO_WRITE (0x51): drive a proxied pad (open-drain semantics in the HAL). */
static alp_cc3501e_resp_t handle_gpio_write(const uint8_t *req,
                                            size_t         req_len,
                                            uint8_t       *reply_data,
                                            size_t         reply_cap,
                                            size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != sizeof(alp_cc3501e_gpio_write_t)) return ALP_CC3501E_RESP_ERR_INVALID;
	const alp_cc3501e_gpio_write_t *w = (const alp_cc3501e_gpio_write_t *)req;
	return hw_to_resp(cc3501e_hw_gpio_write(w->cc3501e_gpio, w->level));
}

/* GPIO_READ (0x52): request payload = 1 byte (cc3501e_gpio); reply data =
 * the sampled level (1 byte). */
static alp_cc3501e_resp_t handle_gpio_read(const uint8_t *req,
                                           size_t         req_len,
                                           uint8_t       *reply_data,
                                           size_t         reply_cap,
                                           size_t        *reply_data_len)
{
	*reply_data_len = 0u;
	if (req_len != 1u) return ALP_CC3501E_RESP_ERR_INVALID;
	if (reply_cap < 1u) return ALP_CC3501E_RESP_ERR_NO_MEM;
	uint8_t            level = 0u;
	alp_cc3501e_resp_t st    = hw_to_resp(cc3501e_hw_gpio_read(req[0], &level));
	if (st == ALP_CC3501E_RESP_OK) {
		reply_data[0]   = level;
		*reply_data_len = 1u;
	}
	return st;
}

/* GPIO_SET_INTERRUPT (0x53): arm/disable an edge IRQ on a proxied pad.  The
 * async EVT_GPIO_INTERRUPT delivery needs the next-rev host-IRQ line; this
 * rev accepts the config (event delivery lands with r2 -- see DESIGN.md). */
static alp_cc3501e_resp_t handle_gpio_set_interrupt(const uint8_t *req,
                                                    size_t         req_len,
                                                    uint8_t       *reply_data,
                                                    size_t         reply_cap,
                                                    size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != sizeof(alp_cc3501e_gpio_set_interrupt_t)) return ALP_CC3501E_RESP_ERR_INVALID;
	const alp_cc3501e_gpio_set_interrupt_t *s = (const alp_cc3501e_gpio_set_interrupt_t *)req;
	return hw_to_resp(cc3501e_hw_gpio_set_interrupt(s->cc3501e_gpio, s->edge, s->enabled));
}

/* CAM_ENABLE (0x60) / CAM_DISABLE (0x61): drive CAM_EN_LDO0/1.  Request
 * payload = 1 byte (which: 0 -> LDO0, 1 -> LDO1). */
static alp_cc3501e_resp_t handle_cam_enable(const uint8_t *req,
                                            size_t         req_len,
                                            uint8_t       *reply_data,
                                            size_t         reply_cap,
                                            size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 1u) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_cam_enable(req[0], 1u));
}

static alp_cc3501e_resp_t handle_cam_disable(const uint8_t *req,
                                             size_t         req_len,
                                             uint8_t       *reply_data,
                                             size_t         reply_cap,
                                             size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 1u) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_cam_enable(req[0], 0u));
}

/* --------------------------------------------------------------- */
/* Wi-Fi (0x10..0x1F)                                                */
/* --------------------------------------------------------------- */

/* WIFI_SCAN_START (0x10): reply data = the packed AP-record list (each
 * record = bssid[6] | rssi(1) | channel(1) | security(1) | ssid_len(1)
 * then ssid_len SSID bytes -- the cc3501e_wifi_scan host parser's wire
 * format).  Worker-routed: the real Wlan_Scan + event rendezvous blocks
 * for seconds and MUST run off the SPI ISR (see handle_worker_routed). */
static alp_cc3501e_resp_t handle_wifi_scan_start(const uint8_t *req,
                                                 size_t         req_len,
                                                 uint8_t       *reply_data,
                                                 size_t         reply_cap,
                                                 size_t        *reply_data_len)
{
	(void)req;
	return handle_worker_routed(
	    ALP_CC3501E_CMD_WIFI_SCAN_START, 0u, req_len, reply_data, reply_cap, reply_data_len);
}

static alp_cc3501e_resp_t handle_wifi_scan_stop(const uint8_t *req,
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
static alp_cc3501e_resp_t wifi_join(const uint8_t *req, size_t req_len, int ap)
{
	if (req_len < sizeof(alp_cc3501e_wifi_connect_t)) return ALP_CC3501E_RESP_ERR_INVALID;
	const alp_cc3501e_wifi_connect_t *c = (const alp_cc3501e_wifi_connect_t *)req;
	const size_t                      need =
	    sizeof(alp_cc3501e_wifi_connect_t) + (size_t)c->ssid_len + (size_t)c->psk_len;
	if (req_len != need) return ALP_CC3501E_RESP_ERR_INVALID;
	const uint8_t *ssid = req + sizeof(alp_cc3501e_wifi_connect_t);
	const uint8_t *psk  = ssid + c->ssid_len;
	const int      rv =
	    ap ? cc3501e_hw_wifi_ap_start(ssid, c->ssid_len, psk, c->psk_len, c->security)
	       : cc3501e_hw_wifi_connect_sta(ssid, c->ssid_len, psk, c->psk_len, c->security);
	return hw_to_resp(rv);
}

static alp_cc3501e_resp_t handle_wifi_connect_sta(const uint8_t *req,
                                                  size_t         req_len,
                                                  uint8_t       *reply_data,
                                                  size_t         reply_cap,
                                                  size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	return wifi_join(req, req_len, 0);
}

static alp_cc3501e_resp_t handle_wifi_ap_start(const uint8_t *req,
                                               size_t         req_len,
                                               uint8_t       *reply_data,
                                               size_t         reply_cap,
                                               size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	return wifi_join(req, req_len, 1);
}

static alp_cc3501e_resp_t handle_wifi_disconnect(const uint8_t *req,
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

static alp_cc3501e_resp_t handle_wifi_ap_stop(const uint8_t *req,
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
static alp_cc3501e_resp_t handle_wifi_get_rssi(const uint8_t *req,
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
static alp_cc3501e_resp_t handle_wifi_get_ip(const uint8_t *req,
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

/* --------------------------------------------------------------- */
/* BLE 5.4 (0x30..0x4F)                                              */
/*                                                                   */
/* BLE payloads with multi-byte fields are parsed field-by-field from */
/* the PACKED wire (LE), not by casting to the doc structs in         */
/* <alp/protocol/cc3501e.h> -- those structs carry uint16 alignment   */
/* padding that the wire format does not.                             */
/* --------------------------------------------------------------- */

/* BLE_ENABLE (0x30): bring up the BLE stack.  Worker-routed (P0-4 seam): the
 * real CC3501E_BLE body starts Wi-Fi first (shared HIF) then nimble_host_start,
 * which blocks for SECONDS -- MUST NOT run in the SPI ISR that dispatches this
 * handler.  Poll-by-repeat, identical to GET_MAC (see handle_worker_routed).
 * Argless reply (the OK carries no data; min_cap 0). */
static alp_cc3501e_resp_t handle_ble_enable(const uint8_t *req,
                                            size_t         req_len,
                                            uint8_t       *reply_data,
                                            size_t         reply_cap,
                                            size_t        *reply_data_len)
{
	(void)req;
	return handle_worker_routed(
	    ALP_CC3501E_CMD_BLE_ENABLE, 0u, req_len, reply_data, reply_cap, reply_data_len);
}

static alp_cc3501e_resp_t handle_ble_disable(const uint8_t *req,
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
	return hw_to_resp(cc3501e_hw_ble_disable());
}

/* BLE_ADV_START (0x32): packed wire = connectable(1) | reserved(1) |
 * interval_min_ms(LE16) | interval_max_ms(LE16) | adv_data_len(1) |
 * adv_data[adv_data_len].  (7-byte header; the doc struct is 8 with pad.) */
#define BLE_ADV_START_HDR 7u
static alp_cc3501e_resp_t handle_ble_adv_start(const uint8_t *req,
                                               size_t         req_len,
                                               uint8_t       *reply_data,
                                               size_t         reply_cap,
                                               size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len < BLE_ADV_START_HDR) return ALP_CC3501E_RESP_ERR_INVALID;
	const uint8_t  connectable  = req[0];
	const uint16_t interval_min = (uint16_t)req[2] | ((uint16_t)req[3] << 8);
	const uint16_t interval_max = (uint16_t)req[4] | ((uint16_t)req[5] << 8);
	const uint8_t  adv_data_len = req[6];
	if (req_len != (size_t)BLE_ADV_START_HDR + adv_data_len) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_ble_adv_start(
	    connectable, interval_min, interval_max, &req[BLE_ADV_START_HDR], adv_data_len));
}

static alp_cc3501e_resp_t handle_ble_adv_stop(const uint8_t *req,
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
	return hw_to_resp(cc3501e_hw_ble_adv_stop());
}

static alp_cc3501e_resp_t handle_ble_scan_start(const uint8_t *req,
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
	return hw_to_resp(cc3501e_hw_ble_scan_start());
}

static alp_cc3501e_resp_t handle_ble_scan_stop(const uint8_t *req,
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
	return hw_to_resp(cc3501e_hw_ble_scan_stop());
}

/* BLE_CONNECT (0x36): packed wire = addr_type(1) | addr[6]. */
static alp_cc3501e_resp_t handle_ble_connect(const uint8_t *req,
                                             size_t         req_len,
                                             uint8_t       *reply_data,
                                             size_t         reply_cap,
                                             size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 7u) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_ble_connect(req[0], &req[1]));
}

static alp_cc3501e_resp_t handle_ble_disconnect(const uint8_t *req,
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
	return hw_to_resp(cc3501e_hw_ble_disconnect());
}

/* BLE_GATT_REGISTER (0x38): opaque attribute-table descriptor (>= 1 byte). */
static alp_cc3501e_resp_t handle_ble_gatt_register(const uint8_t *req,
                                                   size_t         req_len,
                                                   uint8_t       *reply_data,
                                                   size_t         reply_cap,
                                                   size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len < 1u) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_ble_gatt_register(req, (uint16_t)req_len));
}

/* BLE_GATT_NOTIFY (0x39) / WRITE (0x3B): packed wire = handle(LE16) | data. */
static alp_cc3501e_resp_t handle_ble_gatt_notify(const uint8_t *req,
                                                 size_t         req_len,
                                                 uint8_t       *reply_data,
                                                 size_t         reply_cap,
                                                 size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len < 2u) return ALP_CC3501E_RESP_ERR_INVALID;
	const uint16_t handle = (uint16_t)req[0] | ((uint16_t)req[1] << 8);
	return hw_to_resp(cc3501e_hw_ble_gatt_notify(handle, &req[2], (uint16_t)(req_len - 2u)));
}

static alp_cc3501e_resp_t handle_ble_gatt_write(const uint8_t *req,
                                                size_t         req_len,
                                                uint8_t       *reply_data,
                                                size_t         reply_cap,
                                                size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len < 2u) return ALP_CC3501E_RESP_ERR_INVALID;
	const uint16_t handle = (uint16_t)req[0] | ((uint16_t)req[1] << 8);
	return hw_to_resp(cc3501e_hw_ble_gatt_write(handle, &req[2], (uint16_t)(req_len - 2u)));
}

/* BLE_GATT_READ (0x3A): packed wire = handle(LE16); reply data = attr value. */
static alp_cc3501e_resp_t handle_ble_gatt_read(const uint8_t *req,
                                               size_t         req_len,
                                               uint8_t       *reply_data,
                                               size_t         reply_cap,
                                               size_t        *reply_data_len)
{
	*reply_data_len = 0u;
	if (req_len != 2u) return ALP_CC3501E_RESP_ERR_INVALID;
	const uint16_t     handle  = (uint16_t)req[0] | ((uint16_t)req[1] << 8);
	uint16_t           out_len = 0u;
	alp_cc3501e_resp_t st =
	    hw_to_resp(cc3501e_hw_ble_gatt_read(handle, reply_data, (uint16_t)reply_cap, &out_len));
	if (st == ALP_CC3501E_RESP_OK) *reply_data_len = out_len;
	return st;
}

/* --------------------------------------------------------------- */
/* Power policy + diagnostics (0x04, 0x62, 0x70, 0x71)               */
/* --------------------------------------------------------------- */

/* POWER_POLICY (0x62): packed wire = policy(1) | wake_events(1) |
 * reserved(2) | idle_ms_before_sleep(LE32) = 8 bytes. */
static alp_cc3501e_resp_t handle_power_policy(const uint8_t *req,
                                              size_t         req_len,
                                              uint8_t       *reply_data,
                                              size_t         reply_cap,
                                              size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 8u) return ALP_CC3501E_RESP_ERR_INVALID;
	const uint8_t  policy      = req[0];
	const uint8_t  wake_events = req[1];
	const uint32_t idle_ms = (uint32_t)req[4] | ((uint32_t)req[5] << 8) | ((uint32_t)req[6] << 16) |
	                         ((uint32_t)req[7] << 24);
	return hw_to_resp(cc3501e_hw_set_power_policy(policy, wake_events, idle_ms));
}

/* DIAG_LOG_LEVEL (0x71): packed wire = level(1). */
static alp_cc3501e_resp_t handle_diag_log_level(const uint8_t *req,
                                                size_t         req_len,
                                                uint8_t       *reply_data,
                                                size_t         reply_cap,
                                                size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 1u) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_set_log_level(req[0]));
}

/* DIAG_GET_STATS (0x70): reply data = frames_ok(LE32) | frames_err(LE32). */
static alp_cc3501e_resp_t handle_diag_get_stats(const uint8_t *req,
                                                size_t         req_len,
                                                uint8_t       *reply_data,
                                                size_t         reply_cap,
                                                size_t        *reply_data_len)
{
	(void)req;
	*reply_data_len = 0u;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	if (reply_cap < 8u) return ALP_CC3501E_RESP_ERR_NO_MEM;
	put_le32(&reply_data[0], g_frames_ok);
	put_le32(&reply_data[4], g_frames_err);
	*reply_data_len = 8u;
	return ALP_CC3501E_RESP_OK;
}

/* GET_DIAG_INFO (0x04): reply data = the 16-byte packed diag struct:
 * fw_version(LE16) | reset_cause(1) | role(1) | uptime_ms(LE32) |
 * free_heap_bytes(LE32) | last_error(1) | reserved(3). */
static alp_cc3501e_resp_t handle_get_diag_info(const uint8_t *req,
                                               size_t         req_len,
                                               uint8_t       *reply_data,
                                               size_t         reply_cap,
                                               size_t        *reply_data_len)
{
	(void)req;
	*reply_data_len = 0u;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	if (reply_cap < 16u) return ALP_CC3501E_RESP_ERR_NO_MEM;
	put_le16(&reply_data[0], (uint16_t)CC3501E_BRIDGE_FW_VERSION_U16);
	reply_data[2] = cc3501e_hw_reset_cause();
	reply_data[3] = (uint8_t)ALP_CC3501E_ROLE_OFF; /* v0.1: no radio role active */
	put_le32(&reply_data[4], cc3501e_hw_uptime_ms());
	put_le32(&reply_data[8], cc3501e_hw_free_heap_bytes());
	reply_data[12]  = g_last_error;
	reply_data[13]  = 0u;
	reply_data[14]  = 0u;
	reply_data[15]  = 0u;
	*reply_data_len = 16u;
	return ALP_CC3501E_RESP_OK;
}

/* --------------------------------------------------------------- */
/* Dispatch                                                          */
/* --------------------------------------------------------------- */

typedef alp_cc3501e_resp_t (*cmd_handler_t)(const uint8_t *, size_t, uint8_t *, size_t, size_t *);

/* Sparse switch on opcode -- keeps the table small without losing the
 * single-handler-table property.  v0.2+ feature groups slot in here as
 * their HAL bodies land. */
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
