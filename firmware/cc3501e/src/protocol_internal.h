/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: internal cross-TU plumbing shared by the
 * per-command-family handler files (protocol_<family>.c).
 *
 * protocol.c used to be one 1377-line TU dispatching every command
 * family (meta, stream-write, GPIO, camera, Wi-Fi, sockets, BLE, power,
 * diagnostics, OTA).  It is now split one handler-owning TU per family;
 * this header is the seam those TUs share -- NOT a public interface (it
 * is not installed, not included outside this directory, and carries no
 * wire-protocol semantics of its own).  The wire contract stays sourced
 * from <alp/protocol/cc3501e.h> via protocol.h; this header adds no
 * opcode/response constants of its own.
 *
 * Contents:
 *   - little-endian wire helpers (put_le16/put_le32/get_le32)
 *   - hw_to_resp(): HAL return code -> wire response status
 *   - the worker-routed state-machine helpers (handle_worker_routed and
 *     friends), used by the meta / Wi-Fi / sockets / BLE handlers to
 *     move a blocking HAL body off the SPI ISR -- defined once in
 *     protocol.c (the top-dispatch TU) and declared here
 *   - the diagnostics counters protocol_build_reply() (protocol.c)
 *     updates and protocol_diag.c's DIAG_GET_STATS / GET_DIAG_INFO
 *     handlers read
 */

#ifndef CC3501E_BRIDGE_PROTOCOL_INTERNAL_H
#define CC3501E_BRIDGE_PROTOCOL_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"
#include "worker.h"
#include "../hal/cc3501e_hw.h"

/* --------------------------------------------------------------- */
/* Little-endian helpers (firmware side, parallel to the host).      */
/* --------------------------------------------------------------- */

static inline void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
	p[2] = (uint8_t)((v >> 16) & 0xFFu);
	p[3] = (uint8_t)((v >> 24) & 0xFFu);
}

/* Read a 32-bit little-endian field from the wire byte-by-byte (the request
 * buffer is not guaranteed 4-aligned, so don't cast through a u32 pointer). */
static inline uint32_t get_le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Map a HAL return code (CC3501E_HW_*) onto a wire response status.
 * NOTIMPL -> NOT_READY (the op is not wired on this backend yet);
 * INVAL -> INVALID; any other failure -> RADIO (generic HW/IO). */
static inline alp_cc3501e_resp_t hw_to_resp(int rv)
{
	switch (rv) {
	case CC3501E_HW_OK:
		return ALP_CC3501E_RESP_OK;
	case CC3501E_HW_BUSY:
		return ALP_CC3501E_RESP_ERR_BUSY;
	case CC3501E_HW_ERR_NOTIMPL:
		return ALP_CC3501E_RESP_ERR_NOT_READY;
	case CC3501E_HW_ERR_INVAL:
		return ALP_CC3501E_RESP_ERR_INVALID;
	default:
		return ALP_CC3501E_RESP_ERR_RADIO;
	}
}

/* --------------------------------------------------------------- */
/* Worker-routed state-machine helpers (defined once, in protocol.c) */
/* --------------------------------------------------------------- */

/* Generic worker-routed handler for an ARGUMENT-FREE command whose HAL body
 * may BLOCK for seconds (a radio op) and therefore must NOT run in the SPI
 * ISR that dispatches the handler.  GET_MAC pioneered the seam (P0-4/P0-6);
 * WIFI_SCAN_START and WIFI_GET_RSSI share it -- the only per-command knobs
 * are the opcode and the reply-capacity floor.
 *
 * The command is POLL-BY-REPEAT: the host re-issues it (it maps
 * RESP_ERR_BUSY -> ALP_ERR_BUSY and retries) until the worker has the
 * result.  The state machine:
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
alp_cc3501e_resp_t handle_worker_routed(alp_cc3501e_cmd_t cmd,
                                        size_t            min_cap,
                                        size_t            req_len,
                                        uint8_t          *reply_data,
                                        size_t            reply_cap,
                                        size_t           *reply_data_len);

/* As handle_worker_routed, but for a job that CARRIES a request payload
 * (WIFI_CONNECT_STA / WIFI_AP_START).  The caller must have validated @p req /
 * @p req_len already; on the IDLE edge the payload is stashed via
 * worker_submit_payload so the drain runs the blocking association off the SPI
 * ISR.  These ops carry no reply payload (min_cap 0). */
alp_cc3501e_resp_t handle_worker_routed_payload(alp_cc3501e_cmd_t cmd,
                                                const uint8_t    *req,
                                                size_t            req_len,
                                                size_t           *reply_data_len);

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
                                                      size_t           *reply_data_len);

/* --------------------------------------------------------------- */
/* Per-family command handlers                                       */
/* --------------------------------------------------------------- */
/*
 * Declared here (not `static`) so protocol.c's dispatch switch can call
 * across TUs into protocol_<family>.c.  Definitions + their doc comments
 * live in the owning family file; see the family file for the per-handler
 * contract (request/reply shape, opcode, worker-routing rationale). */

/* protocol_meta.c: PING / GET_VERSION / GET_MAC / RESET / STREAM_WRITE. */
alp_cc3501e_resp_t handle_ping(const uint8_t *req,
                               size_t         req_len,
                               uint8_t       *reply_data,
                               size_t         reply_cap,
                               size_t        *reply_data_len);
alp_cc3501e_resp_t handle_get_version(const uint8_t *req,
                                      size_t         req_len,
                                      uint8_t       *reply_data,
                                      size_t         reply_cap,
                                      size_t        *reply_data_len);
alp_cc3501e_resp_t handle_get_mac(const uint8_t *req,
                                  size_t         req_len,
                                  uint8_t       *reply_data,
                                  size_t         reply_cap,
                                  size_t        *reply_data_len);
alp_cc3501e_resp_t handle_reset(const uint8_t *req,
                                size_t         req_len,
                                uint8_t       *reply_data,
                                size_t         reply_cap,
                                size_t        *reply_data_len);
alp_cc3501e_resp_t handle_stream_write(const uint8_t *req,
                                       size_t         req_len,
                                       uint8_t       *reply_data,
                                       size_t         reply_cap,
                                       size_t        *reply_data_len);

/* protocol_gpio.c: GPIO proxy (0x50..0x5F). */
alp_cc3501e_resp_t handle_gpio_configure(const uint8_t *req,
                                         size_t         req_len,
                                         uint8_t       *reply_data,
                                         size_t         reply_cap,
                                         size_t        *reply_data_len);
alp_cc3501e_resp_t handle_gpio_write(const uint8_t *req,
                                     size_t         req_len,
                                     uint8_t       *reply_data,
                                     size_t         reply_cap,
                                     size_t        *reply_data_len);
alp_cc3501e_resp_t handle_gpio_read(const uint8_t *req,
                                    size_t         req_len,
                                    uint8_t       *reply_data,
                                    size_t         reply_cap,
                                    size_t        *reply_data_len);
alp_cc3501e_resp_t handle_gpio_set_interrupt(const uint8_t *req,
                                             size_t         req_len,
                                             uint8_t       *reply_data,
                                             size_t         reply_cap,
                                             size_t        *reply_data_len);

/* protocol_camera.c: camera enables (0x60..0x61). */
alp_cc3501e_resp_t handle_cam_enable(const uint8_t *req,
                                     size_t         req_len,
                                     uint8_t       *reply_data,
                                     size_t         reply_cap,
                                     size_t        *reply_data_len);
alp_cc3501e_resp_t handle_cam_disable(const uint8_t *req,
                                      size_t         req_len,
                                      uint8_t       *reply_data,
                                      size_t         reply_cap,
                                      size_t        *reply_data_len);

/* protocol_wifi.c: Wi-Fi (0x10..0x1F). */
alp_cc3501e_resp_t handle_wifi_scan_start(const uint8_t *req,
                                          size_t         req_len,
                                          uint8_t       *reply_data,
                                          size_t         reply_cap,
                                          size_t        *reply_data_len);
alp_cc3501e_resp_t handle_wifi_scan_stop(const uint8_t *req,
                                         size_t         req_len,
                                         uint8_t       *reply_data,
                                         size_t         reply_cap,
                                         size_t        *reply_data_len);
alp_cc3501e_resp_t handle_wifi_connect_sta(const uint8_t *req,
                                           size_t         req_len,
                                           uint8_t       *reply_data,
                                           size_t         reply_cap,
                                           size_t        *reply_data_len);
alp_cc3501e_resp_t handle_wifi_ap_start(const uint8_t *req,
                                        size_t         req_len,
                                        uint8_t       *reply_data,
                                        size_t         reply_cap,
                                        size_t        *reply_data_len);
alp_cc3501e_resp_t handle_wifi_disconnect(const uint8_t *req,
                                          size_t         req_len,
                                          uint8_t       *reply_data,
                                          size_t         reply_cap,
                                          size_t        *reply_data_len);
alp_cc3501e_resp_t handle_wifi_ap_stop(const uint8_t *req,
                                       size_t         req_len,
                                       uint8_t       *reply_data,
                                       size_t         reply_cap,
                                       size_t        *reply_data_len);
alp_cc3501e_resp_t handle_wifi_get_rssi(const uint8_t *req,
                                        size_t         req_len,
                                        uint8_t       *reply_data,
                                        size_t         reply_cap,
                                        size_t        *reply_data_len);
alp_cc3501e_resp_t handle_wifi_get_ip(const uint8_t *req,
                                      size_t         req_len,
                                      uint8_t       *reply_data,
                                      size_t         reply_cap,
                                      size_t        *reply_data_len);
alp_cc3501e_resp_t handle_wifi_status(const uint8_t *req,
                                      size_t         req_len,
                                      uint8_t       *reply_data,
                                      size_t         reply_cap,
                                      size_t        *reply_data_len);

/* protocol_sockets.c: TCP/UDP sockets (0x20..0x24). */
alp_cc3501e_resp_t handle_sock_open(const uint8_t *req,
                                    size_t         req_len,
                                    uint8_t       *reply_data,
                                    size_t         reply_cap,
                                    size_t        *reply_data_len);
alp_cc3501e_resp_t handle_sock_connect(const uint8_t *req,
                                       size_t         req_len,
                                       uint8_t       *reply_data,
                                       size_t         reply_cap,
                                       size_t        *reply_data_len);
alp_cc3501e_resp_t handle_sock_send(const uint8_t *req,
                                    size_t         req_len,
                                    uint8_t       *reply_data,
                                    size_t         reply_cap,
                                    size_t        *reply_data_len);
alp_cc3501e_resp_t handle_sock_recv(const uint8_t *req,
                                    size_t         req_len,
                                    uint8_t       *reply_data,
                                    size_t         reply_cap,
                                    size_t        *reply_data_len);
alp_cc3501e_resp_t handle_sock_close(const uint8_t *req,
                                     size_t         req_len,
                                     uint8_t       *reply_data,
                                     size_t         reply_cap,
                                     size_t        *reply_data_len);

/* protocol_ble.c: BLE 5.4 (0x30..0x3F). */
alp_cc3501e_resp_t handle_ble_enable(const uint8_t *req,
                                     size_t         req_len,
                                     uint8_t       *reply_data,
                                     size_t         reply_cap,
                                     size_t        *reply_data_len);
alp_cc3501e_resp_t handle_ble_disable(const uint8_t *req,
                                      size_t         req_len,
                                      uint8_t       *reply_data,
                                      size_t         reply_cap,
                                      size_t        *reply_data_len);
alp_cc3501e_resp_t handle_ble_adv_start(const uint8_t *req,
                                        size_t         req_len,
                                        uint8_t       *reply_data,
                                        size_t         reply_cap,
                                        size_t        *reply_data_len);
alp_cc3501e_resp_t handle_ble_adv_stop(const uint8_t *req,
                                       size_t         req_len,
                                       uint8_t       *reply_data,
                                       size_t         reply_cap,
                                       size_t        *reply_data_len);
alp_cc3501e_resp_t handle_ble_scan_start(const uint8_t *req,
                                         size_t         req_len,
                                         uint8_t       *reply_data,
                                         size_t         reply_cap,
                                         size_t        *reply_data_len);
alp_cc3501e_resp_t handle_ble_scan_stop(const uint8_t *req,
                                        size_t         req_len,
                                        uint8_t       *reply_data,
                                        size_t         reply_cap,
                                        size_t        *reply_data_len);
alp_cc3501e_resp_t handle_ble_connect(const uint8_t *req,
                                      size_t         req_len,
                                      uint8_t       *reply_data,
                                      size_t         reply_cap,
                                      size_t        *reply_data_len);
alp_cc3501e_resp_t handle_ble_disconnect(const uint8_t *req,
                                         size_t         req_len,
                                         uint8_t       *reply_data,
                                         size_t         reply_cap,
                                         size_t        *reply_data_len);
alp_cc3501e_resp_t handle_ble_gatt_register(const uint8_t *req,
                                            size_t         req_len,
                                            uint8_t       *reply_data,
                                            size_t         reply_cap,
                                            size_t        *reply_data_len);
alp_cc3501e_resp_t handle_ble_gatt_notify(const uint8_t *req,
                                          size_t         req_len,
                                          uint8_t       *reply_data,
                                          size_t         reply_cap,
                                          size_t        *reply_data_len);
alp_cc3501e_resp_t handle_ble_gatt_read(const uint8_t *req,
                                        size_t         req_len,
                                        uint8_t       *reply_data,
                                        size_t         reply_cap,
                                        size_t        *reply_data_len);
alp_cc3501e_resp_t handle_ble_gatt_write(const uint8_t *req,
                                         size_t         req_len,
                                         uint8_t       *reply_data,
                                         size_t         reply_cap,
                                         size_t        *reply_data_len);

/* protocol_power.c: power policy (0x62). */
alp_cc3501e_resp_t handle_power_policy(const uint8_t *req,
                                       size_t         req_len,
                                       uint8_t       *reply_data,
                                       size_t         reply_cap,
                                       size_t        *reply_data_len);

/* protocol_diag.c: diagnostics (0x04, 0x05, 0x70, 0x71). */
alp_cc3501e_resp_t handle_diag_log_level(const uint8_t *req,
                                         size_t         req_len,
                                         uint8_t       *reply_data,
                                         size_t         reply_cap,
                                         size_t        *reply_data_len);
alp_cc3501e_resp_t handle_diag_get_stats(const uint8_t *req,
                                         size_t         req_len,
                                         uint8_t       *reply_data,
                                         size_t         reply_cap,
                                         size_t        *reply_data_len);
alp_cc3501e_resp_t handle_get_diag_info(const uint8_t *req,
                                        size_t         req_len,
                                        uint8_t       *reply_data,
                                        size_t         reply_cap,
                                        size_t        *reply_data_len);
alp_cc3501e_resp_t handle_get_pending_events(const uint8_t *req,
                                             size_t         req_len,
                                             uint8_t       *reply_data,
                                             size_t         reply_cap,
                                             size_t        *reply_data_len);

/* protocol_ota.c: OTA firmware update (0x40..0x46). */
alp_cc3501e_resp_t handle_ota_begin(const uint8_t *req,
                                    size_t         req_len,
                                    uint8_t       *reply_data,
                                    size_t         reply_cap,
                                    size_t        *reply_data_len);
alp_cc3501e_resp_t handle_ota_write(const uint8_t *req,
                                    size_t         req_len,
                                    uint8_t       *reply_data,
                                    size_t         reply_cap,
                                    size_t        *reply_data_len);
alp_cc3501e_resp_t handle_ota_finish(const uint8_t *req,
                                     size_t         req_len,
                                     uint8_t       *reply_data,
                                     size_t         reply_cap,
                                     size_t        *reply_data_len);
alp_cc3501e_resp_t handle_ota_abort(const uint8_t *req,
                                    size_t         req_len,
                                    uint8_t       *reply_data,
                                    size_t         reply_cap,
                                    size_t        *reply_data_len);
alp_cc3501e_resp_t handle_ota_promote(const uint8_t *req,
                                      size_t         req_len,
                                      uint8_t       *reply_data,
                                      size_t         reply_cap,
                                      size_t        *reply_data_len);
alp_cc3501e_resp_t handle_ota_status(const uint8_t *req,
                                     size_t         req_len,
                                     uint8_t       *reply_data,
                                     size_t         reply_cap,
                                     size_t        *reply_data_len);

/* --------------------------------------------------------------- */
/* Diagnostics state (firmware-side)                                 */
/* --------------------------------------------------------------- */
/*
 * last_error = the most recent non-OK response emitted; the frame counters
 * feed DIAG_GET_STATS.  All are DEFINED in protocol.c and updated centrally
 * in protocol_build_reply(); protocol_diag.c only reads them (DIAG_GET_STATS,
 * GET_DIAG_INFO). */
extern uint8_t  g_last_error;
extern uint32_t g_frames_ok;
extern uint32_t g_frames_err;

#endif /* CC3501E_BRIDGE_PROTOCOL_INTERNAL_H */
