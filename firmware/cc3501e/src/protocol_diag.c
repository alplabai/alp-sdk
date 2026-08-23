/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: diagnostics command-family handlers
 * (0x04, 0x05, 0x70, 0x71).  Split out of protocol.c (issue #461);
 * protocol_dispatch() in protocol.c still owns the single
 * command-family switch that routes here.
 *
 * g_last_error / g_frames_ok / g_frames_err are DEFINED in protocol.c
 * (updated centrally in protocol_build_reply()); this file only reads
 * them via the extern declarations in protocol_internal.h.
 */

#include "protocol_internal.h"
#include "event_ring.h"

/* Firmware *release* version reported by GET_DIAG_INFO.fw_version (u16).
 *
 * SINGLE SOURCE OF TRUTH = firmware-version.txt.  The build systems parse
 * that SemVer and pass the packed value in on the command line, so the
 * runtime marker can never drift from the release version:
 *   - CMake (stub build):  target_compile_definitions in CMakeLists.txt
 *   - ti build:            -DCC3501E_BRIDGE_FW_VERSION_U16 in ti/build_ti.sh
 *
 * Packing (pre-1.0 SemVer 0.MINOR.PATCH): high byte = MINOR, low byte =
 * PATCH, i.e. 0.2.0 -> 0x0200, 0.2.5 -> 0x0205.  MAJOR is 0 pre-1.0; fold
 * it into the encoding when the first 1.x ships.
 *
 * This is distinct from BOTH:
 *   - ALP_CC3501E_PROTOCOL_VERSION (GET_VERSION) -- the wire-compat gate.
 *   - the GPE flash/image version (ti/deploy_validate.sh) -- the vendor-RoT
 *     anti-rollback gate; monotonic + date-derived, NOT this app SemVer.
 *
 * The #ifndef fallback keeps a standalone compile (no -D) sane and MUST
 * track firmware-version.txt's current value. */
#ifndef CC3501E_BRIDGE_FW_VERSION_U16
#define CC3501E_BRIDGE_FW_VERSION_U16 0x0200u /* fallback = firmware-version.txt 0.2.0 */
#endif

/* DIAG_LOG_LEVEL (0x71): packed wire = level(1). */
alp_cc3501e_resp_t handle_diag_log_level(const uint8_t *req,
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
alp_cc3501e_resp_t handle_diag_get_stats(const uint8_t *req,
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
alp_cc3501e_resp_t handle_get_diag_info(const uint8_t *req,
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
	/* Real Wi-Fi role (was hardcoded ROLE_OFF behind a stale "v0.1: no radio
	 * role active" comment).  This is the field #1562 needs: with the AP role
	 * latched here, a host polling GET_DIAG_INFO can tell "the firmware tore the
	 * AP down" (role goes OFF) from "the NWP stopped beaconing while we still
	 * believe the role is up" (role stays WIFI_AP, no beacon on a second radio).
	 * Wi-Fi only -- see cc3501e_hw_radio_role()'s SCOPE note; BLE is not folded in. */
	reply_data[3] = cc3501e_hw_radio_role();
	put_le32(&reply_data[4], cc3501e_hw_uptime_ms());
	/* Actual free heap again.  This field carried cc3501e_hw_wifi_last_event_id()
	 * behind a "DEBUG: was free_heap" comment, so `alp companion diag info`
	 * rendered an event ID as "heap: 0 B free" -- alarming, and false.  The event
	 * ID keeps its diagnostic value at reserved[0] below, where it is not
	 * impersonating a documented field. */
	put_le32(&reply_data[8], cc3501e_hw_free_heap_bytes());
	reply_data[12] = g_last_error;
	/* reserved[0]: last Wi-Fi event ID (0 = no WLAN event has ever fired).  Kept
	 * because it earned its place on the bench -- an ap_start that leaves this at
	 * 0 never got a WLAN event at all, which is a far sharper signal than "the
	 * SSID did not appear on another radio" and needs no second radio. Additive
	 * into a reserved byte: a host that does not know about it still reads 0,
	 * which is what the old firmware put there. */
	reply_data[13]  = (uint8_t)(cc3501e_hw_wifi_last_event_id() & 0xFFu);
	reply_data[14]  = 0u;
	reply_data[15]  = 0u;
	*reply_data_len = 16u;
	return ALP_CC3501E_RESP_OK;
}

/* GET_PENDING_EVENTS (0x05): drain the async-event ring into the reply.  Reply
 * data is a packed list of { evt_opcode(1) | len(1) | payload[len] } entries
 * (alp_cc3501e_event_entry_t); an empty ring yields zero data bytes (status
 * OK).  Fast + non-blocking -- just an IRQ-safe memcpy out of the ring -- so it
 * runs INLINE in the SPI-ISR dispatch (unlike the seconds-long radio getters,
 * which the worker routes off-ISR).  The producers (the Wi-Fi connect/disconnect
 * path, etc.) push into the ring from thread context. */
alp_cc3501e_resp_t handle_get_pending_events(const uint8_t *req,
                                             size_t         req_len,
                                             uint8_t       *reply_data,
                                             size_t         reply_cap,
                                             size_t        *reply_data_len)
{
	(void)req;
	*reply_data_len = 0u;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	*reply_data_len = event_ring_drain(reply_data, reply_cap);
	return ALP_CC3501E_RESP_OK;
}
