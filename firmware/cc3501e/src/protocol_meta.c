/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: META command-family handlers (PING,
 * GET_VERSION, GET_MAC, RESET) plus the proto-v2 bulk-data STREAM_WRITE
 * sink.  Split out of protocol.c (issue #461); protocol_dispatch() in
 * protocol.c still owns the single command-family switch that routes
 * here.
 *
 * v0.1 ("bring-up") scope: the META group only.  Every other opcode --
 * Wi-Fi, BLE, GPIO proxy, power, diagnostics -- is rejected with
 * ALP_CC3501E_RESP_ERR_INVALID per the protocol header's contract ("v1
 * firmware rejects opcodes it does not implement with
 * ALP_CC3501E_RESP_ERR_INVALID").  Those land in v0.2+ and route to
 * TI's SimpleLink Wi-Fi / BLE APIs through the HAL backend; see
 * docs/cc3501e-bridge.md "v0.x roadmap".
 *
 * Handlers that touch hardware (MAC read, self-reset) call the
 * cc3501e_hw_* shims declared in ../hal/cc3501e_hw.h.  The stub backend
 * (hal/cc3501e_hw_stub.c) keeps the protocol path exercisable on the
 * host with no TI SDK; the real bodies live in hal/ti/ and link against
 * TI's SimpleLink CC33xx SDK on the bench build.
 */

#include "protocol_internal.h"

/* Running total of bytes sunk by CMD_STREAM_WRITE (proto v2 bulk stream). */
static uint32_t g_stream_bytes;

/* --------------------------------------------------------------- */
/* META handlers (opcodes 0x00..0x0F)                                */
/* --------------------------------------------------------------- */

/* PING (0x00): liveness probe.  Empty request, empty reply data --
 * the bare RESP_OK status the transport prepends is the "firmware is
 * alive" signal the host's first post-boot handshake waits for. */
alp_cc3501e_resp_t handle_ping(const uint8_t *req,
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
alp_cc3501e_resp_t handle_get_version(const uint8_t *req,
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

/* STREAM_WRITE (0x45, proto v2): bulk-data stream sink.  Accepts an opaque
 * payload (up to ALP_CC3501E_MAX_PAYLOAD-header bytes), counts + discards it,
 * and acks with empty reply data.  A back-to-back sequence is a FRAMED bulk
 * stream whose per-frame payload phase rides the host DMA path; because every
 * frame is acked the link never desyncs (unlike raw throwaway clocking).  The
 * running total is reported via GET_DIAG_INFO for throughput accounting.
 * Synchronous (no worker): a memory sink can't block. */
alp_cc3501e_resp_t handle_stream_write(const uint8_t *req,
                                       size_t         req_len,
                                       uint8_t       *reply_data,
                                       size_t         reply_cap,
                                       size_t        *reply_data_len)
{
	(void)req;
	(void)reply_data;
	(void)reply_cap;
	g_stream_bytes += (uint32_t)req_len;
	*reply_data_len = 0u;
	return ALP_CC3501E_RESP_OK;
}

/* GET_MAC (0x03): the CC3501E's factory MAC (6 bytes, big-endian wire
 * order as TI stores it).  Read from the radio subsystem via the HAL.
 *
 * Routed through the async worker (P0-4/P0-6): the real CC3501E_WIFI body
 * (Wlan_Get, preceded by a one-time Wi-Fi init) blocks for SECONDS, which
 * MUST NOT happen in the SPI ISR that runs this handler.  See
 * handle_worker_routed for the poll-by-repeat state machine. */
alp_cc3501e_resp_t handle_get_mac(const uint8_t *req,
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
alp_cc3501e_resp_t handle_reset(const uint8_t *req,
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
