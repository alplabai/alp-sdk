/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cc3501e.h
 * @brief Wire protocol between Alif Ensemble and the on-module
 *        TI CC3501E Wi-Fi 6 + BLE 5.4 coprocessor.
 *
 * The CC3501E ships its own Cortex-M MCU and runs ALP-authored
 * firmware that lives in this repo at `firmware/cc3501e/` (embedded,
 * like the gd32-bridge -- see ADR 0015).  The firmware exposes Wi-Fi +
 * BLE control to the Alif over the inter-chip link (SPI default, SDIO
 * optional) -- Alif is master, CC3501E is slave.  This header is the
 * single-source contract between the two sides: the firmware includes
 * it directly, so a change here moves both sides in one commit.
 *
 * Why a custom protocol instead of standardised Wi-Fi-host
 * commands (e.g. ESP-AT)?  We need granular control over the BLE
 * GATT path + the GPIO proxy (the CC3501E drives IO11 / IO13 /
 * IO15..IO21 + the camera-enable LDOs); ESP-AT is Wi-Fi-only.
 * The protocol is intentionally small and binary -- no AT
 * tokenisation, no escape sequences -- because the channel is a
 * hardwired SPI bus, not a UART-on-headphone-jack.
 *
 * Frame format (little-endian where applicable):
 *
 *   +--------+--------+--------+--------+========+========+
 *   |  cmd   |  flags | payload_len_lo  | payload (N B)   |
 *   +--------+--------+--------+--------+========+========+
 *
 *   cmd          one of `alp_cc3501e_cmd_t`
 *   flags        bit 0 = response-required
 *                bit 1 = async-event payload (slave -> master)
 *                bits 2..7 reserved
 *   payload_len  16-bit LE; total frame = 4 + payload_len
 *
 * Responses use the same shape -- the slave sets flags bit 1
 * (async event) for unsolicited notifications (incoming
 * advertisement, BLE connection event, Wi-Fi disconnect) so the
 * Alif's SPI master can demux solicited replies from events on
 * the same MISO line.
 */

#ifndef ALP_PROTOCOL_CC3501E_H
#define ALP_PROTOCOL_CC3501E_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALP_CC3501E_PROTOCOL_VERSION 1

/** Frame header in bytes, before the payload. */
#define ALP_CC3501E_HEADER_BYTES 4

/** Maximum payload size per frame.  Larger transactions must split
 *  across multiple frames using the FRAME_CONTINUATION flag (bit 2,
 *  reserved in v1; v2 will land alongside the BLE long-write path). */
#define ALP_CC3501E_MAX_PAYLOAD 512

/** Flags bitfield. */
#define ALP_CC3501E_FLAG_RESP_REQUIRED 0x01
#define ALP_CC3501E_FLAG_ASYNC_EVENT   0x02
/** Reserved frame-continuation flag for v2 long-write payloads.
 *  Hosts MUST treat this bit as zero on v1; v2 firmware will set
 *  it on intermediate frames of a multi-frame BLE-write transaction. */
#define ALP_CC3501E_FLAG_CONTINUATION 0x04

/** Marker for the first opcode in the vendor-extension reserved range.
 *  Opcodes >= this value are NOT used by the v1 protocol and are
 *  reserved for future vendor extensions; the firmware-side parser
 *  rejects them with ALP_CC3501E_RESP_ERR_INVALID until a follow-up
 *  protocol revision (v2+) consumes the range. */
#define ALP_CC3501E_CMD_RESERVED_VENDOR_BASE 0x80u

/** Header-idle sync marker driven by the slave on MISO whenever it is
 *  parked at a clean frame boundary (armed for a request header), and
 *  used by the host to (re)establish byte alignment on the CS-less 3-wire
 *  link.  RESERVED: no opcode and no reply-header byte may equal this
 *  value -- it lies in the reserved vendor range (>= 0x80) and a reply
 *  header's first byte always echoes the request's (in-range) opcode, so
 *  0xA5 can never appear as a legitimate header byte.  The host clocks
 *  dummy bytes and looks for a run of 0xA5 to know the slave is at a frame
 *  boundary (see chips/cc3501e/cc3501e.c cc3501e_sync()). */
#define ALP_CC3501E_SYNC_IDLE 0xA5u

/**
 * @brief Command opcodes.
 *
 * Numbering is grouped by subsystem so additions don't perturb the
 * existing range and so the firmware's dispatch table stays sparse-
 * friendly:
 *
 *   0x00..0x0F  meta (ping, version, reset)
 *   0x10..0x2F  Wi-Fi
 *   0x30..0x4F  BLE
 *   0x50..0x5F  GPIO proxy
 *   0x60..0x6F  power / camera-enable
 *   0x70..0x7F  diagnostics
 *   0x80..0xFF  reserved (vendor extensions)
 */
typedef enum {
	/* Meta */
	ALP_CC3501E_CMD_PING        = 0x00,
	ALP_CC3501E_CMD_GET_VERSION = 0x01,
	ALP_CC3501E_CMD_RESET       = 0x02,
	ALP_CC3501E_CMD_GET_MAC     = 0x03,
	/* §5.4 -- extended diagnostics.  Reply payload is
     * @ref alp_cc3501e_diag_info_t.  Adds firmware-side context
     * (reset cause, current role, uptime, free heap, last error)
     * beyond what GET_VERSION returns.  v2-firmware-only; v1
     * firmware rejects with ALP_CC3501E_RESP_ERR_INVALID. */
	ALP_CC3501E_CMD_GET_DIAG_INFO = 0x04,

	/* Wi-Fi */
	ALP_CC3501E_CMD_WIFI_SCAN_START   = 0x10,
	ALP_CC3501E_CMD_WIFI_SCAN_STOP    = 0x11,
	ALP_CC3501E_CMD_WIFI_CONNECT_STA  = 0x12,
	ALP_CC3501E_CMD_WIFI_DISCONNECT   = 0x13,
	ALP_CC3501E_CMD_WIFI_AP_START     = 0x14,
	ALP_CC3501E_CMD_WIFI_AP_STOP      = 0x15,
	ALP_CC3501E_CMD_WIFI_GET_RSSI     = 0x16,
	ALP_CC3501E_CMD_WIFI_GET_IP       = 0x17,
	ALP_CC3501E_EVT_WIFI_SCAN_RESULT  = 0x18, /* async, slave -> master */
	ALP_CC3501E_EVT_WIFI_CONNECTED    = 0x19, /* async */
	ALP_CC3501E_EVT_WIFI_DISCONNECTED = 0x1A, /* async */

	/* TCP/UDP sockets (host-managed; offload to firmware). */
	ALP_CC3501E_CMD_SOCK_OPEN    = 0x20,
	ALP_CC3501E_CMD_SOCK_CONNECT = 0x21,
	ALP_CC3501E_CMD_SOCK_SEND    = 0x22,
	ALP_CC3501E_CMD_SOCK_RECV    = 0x23,
	ALP_CC3501E_CMD_SOCK_CLOSE   = 0x24,

	/* BLE */
	ALP_CC3501E_CMD_BLE_ENABLE         = 0x30,
	ALP_CC3501E_CMD_BLE_DISABLE        = 0x31,
	ALP_CC3501E_CMD_BLE_ADV_START      = 0x32,
	ALP_CC3501E_CMD_BLE_ADV_STOP       = 0x33,
	ALP_CC3501E_CMD_BLE_SCAN_START     = 0x34,
	ALP_CC3501E_CMD_BLE_SCAN_STOP      = 0x35,
	ALP_CC3501E_CMD_BLE_CONNECT        = 0x36,
	ALP_CC3501E_CMD_BLE_DISCONNECT     = 0x37,
	ALP_CC3501E_CMD_BLE_GATT_REGISTER  = 0x38,
	ALP_CC3501E_CMD_BLE_GATT_NOTIFY    = 0x39,
	ALP_CC3501E_CMD_BLE_GATT_READ      = 0x3A,
	ALP_CC3501E_CMD_BLE_GATT_WRITE     = 0x3B,
	ALP_CC3501E_EVT_BLE_ADV_REPORT     = 0x3C, /* async */
	ALP_CC3501E_EVT_BLE_CONNECTED      = 0x3D, /* async */
	ALP_CC3501E_EVT_BLE_DISCONNECTED   = 0x3E, /* async */
	ALP_CC3501E_EVT_BLE_GATT_WRITE_REQ = 0x3F, /* async */

	/* OTA firmware update (over-the-bridge).  The Alif host streams a signed
	 * GPE-format vendor image into the CC3501E's NON-primary vendor slot via
	 * PSA-FWU; on FINISH the CC35 installs + reboots so the cold BL2/MCUboot
	 * swaps the slot to primary (TRIAL boot), and the swapped image accepts
	 * itself (cc3501e_hw_tick).  Streamed sequentially: BEGIN(total_len) ->
	 * WRITE(offset,bytes)* -> FINISH.  See docs/cc3501e-bridge.md "OTA" + the
	 * device-side Mender contract (the OTA server is a separate repo). */
	ALP_CC3501E_CMD_OTA_BEGIN  = 0x40, /* req alp_cc3501e_ota_begin_t        */
	ALP_CC3501E_CMD_OTA_WRITE  = 0x41, /* req alp_cc3501e_ota_write_t + bytes */
	ALP_CC3501E_CMD_OTA_FINISH = 0x42, /* no payload; install + deferred reboot */
	ALP_CC3501E_CMD_OTA_ABORT  = 0x43, /* no payload; cancel the session      */
	ALP_CC3501E_CMD_OTA_STATUS = 0x44, /* reply alp_cc3501e_ota_status_t      */

	/* GPIO proxy.  IO11 / IO13 / IO15..IO21 hang off CC3501E
     * GPIOs; these commands let the Alif read/write them via the
     * inter-chip bus. */
	ALP_CC3501E_CMD_GPIO_CONFIGURE     = 0x50,
	ALP_CC3501E_CMD_GPIO_WRITE         = 0x51,
	ALP_CC3501E_CMD_GPIO_READ          = 0x52,
	ALP_CC3501E_CMD_GPIO_SET_INTERRUPT = 0x53,
	ALP_CC3501E_EVT_GPIO_INTERRUPT     = 0x54, /* async */

	/* Power / camera enables.  CC3501E drives the camera-LDO
     * enable pins (CAM_EN_LDO0/1) per the inter-chip TSV. */
	ALP_CC3501E_CMD_CAM_ENABLE  = 0x60,
	ALP_CC3501E_CMD_CAM_DISABLE = 0x61,
	/* §5.7 -- system-wide power policy for the CC3501E itself.
     * Request payload is @ref alp_cc3501e_power_policy_t.  Lets
     * the host hint at how aggressively the CC3501E firmware
     * should idle between Wi-Fi / BLE events.  v2-firmware-only. */
	ALP_CC3501E_CMD_POWER_POLICY = 0x62,

	/* Diagnostics */
	ALP_CC3501E_CMD_DIAG_GET_STATS = 0x70,
	ALP_CC3501E_CMD_DIAG_LOG_LEVEL = 0x71,
} alp_cc3501e_cmd_t;

/**
 * @brief Response status codes carried in the first byte of every
 *        response payload.  Maps cleanly onto the SDK's alp_status_t
 *        when the host adapts the value.
 */
typedef enum {
	ALP_CC3501E_RESP_OK            = 0x00,
	ALP_CC3501E_RESP_ERR_INVALID   = 0x01, /**< Bad cmd / bad payload. */
	ALP_CC3501E_RESP_ERR_BUSY      = 0x02, /**< Subsystem in use. */
	ALP_CC3501E_RESP_ERR_TIMEOUT   = 0x03,
	ALP_CC3501E_RESP_ERR_NO_MEM    = 0x04,
	ALP_CC3501E_RESP_ERR_NOT_READY = 0x05, /**< Wi-Fi/BLE not enabled. */
	ALP_CC3501E_RESP_ERR_RADIO     = 0x06, /**< RF / antenna failure. */
	ALP_CC3501E_RESP_ERR_PROTOCOL  = 0x07, /**< Frame mis-parse. */
	ALP_CC3501E_RESP_ERR_VERSION   = 0x08, /**< Firmware ↔ host version mismatch. */
	ALP_CC3501E_RESP_ERR_INTERNAL  = 0xFF
} alp_cc3501e_resp_t;

/* ------------------------------------------------------------------ */
/* Meta payload formats                                                */
/* ------------------------------------------------------------------ */

/** Reset-cause codes for @ref alp_cc3501e_diag_info_t::reset_cause.
 *  Field-level meanings:
 *   - UNKNOWN: firmware lost track of the cause.
 *   - POWER_ON: cold boot from PMIC ramp.
 *   - NRST_PIN: host-driven nRESET edge.
 *   - SOFT: host-issued CMD_RESET.
 *   - WATCHDOG: firmware watchdog timeout.
 *   - BROWNOUT: PMIC under-voltage event.
 *   - BLE_STACK: BLE stack panic.
 *   - WIFI_STACK: Wi-Fi stack panic. */
typedef enum {
	ALP_CC3501E_RESET_UNKNOWN    = 0u,
	ALP_CC3501E_RESET_POWER_ON   = 1u,
	ALP_CC3501E_RESET_NRST_PIN   = 2u,
	ALP_CC3501E_RESET_SOFT       = 3u,
	ALP_CC3501E_RESET_WATCHDOG   = 4u,
	ALP_CC3501E_RESET_BROWNOUT   = 5u,
	ALP_CC3501E_RESET_BLE_STACK  = 6u,
	ALP_CC3501E_RESET_WIFI_STACK = 7u,
} alp_cc3501e_reset_cause_t;

/** Active-role codes for @ref alp_cc3501e_diag_info_t::role.
 *  Field-level meanings:
 *   - OFF: radios disabled (deep idle).
 *   - WIFI_STA: Wi-Fi station mode.
 *   - WIFI_AP: Wi-Fi soft-AP mode.
 *   - BLE_PERIPHERAL: BLE peripheral role.
 *   - BLE_CENTRAL: BLE central role.
 *   - DUAL_WIFI_BLE: Wi-Fi STA + BLE coexist. */
typedef enum {
	ALP_CC3501E_ROLE_OFF            = 0u,
	ALP_CC3501E_ROLE_WIFI_STA       = 1u,
	ALP_CC3501E_ROLE_WIFI_AP        = 2u,
	ALP_CC3501E_ROLE_BLE_PERIPHERAL = 3u,
	ALP_CC3501E_ROLE_BLE_CENTRAL    = 4u,
	ALP_CC3501E_ROLE_DUAL_WIFI_BLE  = 5u,
} alp_cc3501e_role_t;

/** Reply payload for CMD_GET_DIAG_INFO (opcode 0x04).  Firmware
 *  populates these fields once per request from its in-RAM
 *  bookkeeping; reading is non-disturbing (no side effects on
 *  the radio state).  Sized at 16 bytes (one cache line on the
 *  M33) so the SPI reply fits in a single short envelope.
 *  Field-level meanings:
 *   - fw_version: the firmware *release* version the device reports
 *     (its own semver from firmware-version.txt; tracked separately
 *     from ALP_CC3501E_PROTOCOL_VERSION).  NOTE: this is distinct from
 *     what CMD_GET_VERSION returns -- GET_VERSION returns the *protocol*
 *     version (ALP_CC3501E_PROTOCOL_VERSION) for the host compatibility
 *     gate; the release version is surfaced only here (v2 firmware).
 *   - reset_cause: one of @ref alp_cc3501e_reset_cause_t.
 *   - role: one of @ref alp_cc3501e_role_t.
 *   - uptime_ms: time since power-on / last reset.
 *   - free_heap_bytes: firmware-allocator free pool.
 *   - last_error: last @ref alp_cc3501e_resp_t the firmware
 *     emitted on the wire; @ref ALP_CC3501E_RESP_OK if no error
 *     since last reset. */
typedef struct {
	uint16_t fw_version;
	uint8_t  reset_cause;
	uint8_t  role;
	uint32_t uptime_ms;
	uint32_t free_heap_bytes;
	uint8_t  last_error;
	uint8_t  reserved[3];
} alp_cc3501e_diag_info_t;

/* ------------------------------------------------------------------ */
/* Power policy payload formats                                        */
/* ------------------------------------------------------------------ */

/** Coarse policy preset for @ref alp_cc3501e_power_policy_t::policy.
 *  Backends round to the closest firmware-supported policy; the
 *  realised policy is reported back via GET_DIAG_INFO if needed.
 *  Field-level meanings:
 *   - PERFORMANCE: no idle; lowest latency.
 *   - BALANCED: default -- idle between events.
 *   - LOW_POWER: aggressive idle; longer wake.
 *   - DEEP_SLEEP: radios off; wake-on-host only. */
typedef enum {
	ALP_CC3501E_PP_PERFORMANCE = 0u,
	ALP_CC3501E_PP_BALANCED    = 1u,
	ALP_CC3501E_PP_LOW_POWER   = 2u,
	ALP_CC3501E_PP_DEEP_SLEEP  = 3u,
} alp_cc3501e_pp_preset_t;

/** Wake-event bitmap for @ref alp_cc3501e_power_policy_t::wake_events.
 *  Bits enabled here keep the CC3501E from idling its respective
 *  receive path; bits cleared let the firmware gate that path while
 *  idle.  All-zeros is valid only with PERFORMANCE / BALANCED policies.
 *  Bit-level meanings:
 *   - HOST_SPI: SPI CS edge from host.
 *   - BLE_CONN: connected BLE traffic.
 *   - BLE_ADV: advertising scanner / responder.
 *   - WIFI_BEACON: Wi-Fi STA beacon listen.
 *   - WIFI_AP_CLIENT: Wi-Fi AP client join / leave.
 *   - GPIO_IRQ: configured GPIO IRQ from CMD_GPIO_SET_INTERRUPT. */
#define ALP_CC3501E_WAKE_NONE           0x00u
#define ALP_CC3501E_WAKE_HOST_SPI       0x01u
#define ALP_CC3501E_WAKE_BLE_CONN       0x02u
#define ALP_CC3501E_WAKE_BLE_ADV        0x04u
#define ALP_CC3501E_WAKE_WIFI_BEACON    0x08u
#define ALP_CC3501E_WAKE_WIFI_AP_CLIENT 0x10u
#define ALP_CC3501E_WAKE_GPIO_IRQ       0x20u

/** Payload of CMD_POWER_POLICY (opcode 0x62).  Hint to the CC3501E
 *  firmware about how aggressively to idle between events.  Takes
 *  effect on the next idle-detection cycle (firmware-defined; ~ms).
 *  Field-level meanings:
 *   - policy: one of @ref alp_cc3501e_pp_preset_t.
 *   - wake_events: bitmap of @c ALP_CC3501E_WAKE_* values.
 *   - idle_ms_before_sleep: minimum idle time before entering the
 *     chosen policy's sleep mode.  0 = use firmware default for
 *     the policy. */
typedef struct {
	uint8_t  policy;
	uint8_t  wake_events;
	uint16_t reserved;
	uint32_t idle_ms_before_sleep;
} alp_cc3501e_power_policy_t;

/* ------------------------------------------------------------------ */
/* Wi-Fi STA payload formats                                          */
/* ------------------------------------------------------------------ */

/** Sent in the payload of CMD_WIFI_CONNECT_STA.  ssid_len + psk_len
 *  upper-bound on the cumulative frame length (still ≤ MAX_PAYLOAD). */
typedef struct {
	uint8_t ssid_len;
	uint8_t psk_len;
	uint8_t security; /**< 0 = open, 1 = WPA2-PSK, 2 = WPA3-SAE */
	uint8_t reserved;
	/* uint8_t ssid[ssid_len];   -- packed inline, no padding */
	/* uint8_t psk[psk_len];     -- packed inline, no padding */
} alp_cc3501e_wifi_connect_t;

/** Async event for CMD_WIFI_SCAN_START and friends. */
typedef struct {
	uint8_t bssid[6];
	int8_t  rssi_dbm;
	uint8_t channel;
	uint8_t security;
	uint8_t ssid_len;
	/* uint8_t ssid[ssid_len]; */
} alp_cc3501e_scan_result_t;

/* ------------------------------------------------------------------ */
/* TCP/UDP socket payload formats                                      */
/* ------------------------------------------------------------------ */

/** Socket type for @ref alp_cc3501e_sock_open_t::type.  Stored on the
 *  wire as a single byte; the named values keep callers from shipping
 *  magic numbers.  Mirrors the BSD SOCK_* split the firmware's IP stack
 *  exposes.
 *  Field-level meanings:
 *   - STREAM: connection-oriented (TCP).
 *   - DGRAM:  connectionless datagram (UDP). */
typedef enum {
	ALP_CC3501E_SOCK_TYPE_STREAM = 0u,
	ALP_CC3501E_SOCK_TYPE_DGRAM  = 1u,
} alp_cc3501e_sock_type_t;

/** Address family for @ref alp_cc3501e_sock_open_t::family and the
 *  family-tag byte of @ref alp_cc3501e_sock_addr_t.  Stored on the wire
 *  as a single byte.  IPv6 is reserved in v1 (the firmware IP stack is
 *  IPv4-only this rev; v2 firmware lands the IPv6 path).
 *  Field-level meanings:
 *   - IPV4: 32-bit IPv4 address in @ref alp_cc3501e_sock_addr_t::addr.
 *   - IPV6: 128-bit IPv6 address (reserved in v1). */
typedef enum {
	ALP_CC3501E_SOCK_FAMILY_IPV4 = 0u,
	ALP_CC3501E_SOCK_FAMILY_IPV6 = 1u,
} alp_cc3501e_sock_family_t;

/** Endpoint address shared by CMD_SOCK_CONNECT and the from-address of
 *  a received datagram.  Fixed 16-byte body so an IPv6 address fits the
 *  same slot once v2 firmware enables it; v1 firmware uses only the
 *  first 4 bytes of @c addr (IPv4) and zero-fills the rest.
 *  Field-level meanings:
 *   - family: one of @ref alp_cc3501e_sock_family_t.
 *   - port: TCP/UDP port, host byte order on this side; the firmware
 *     converts to network order on the wire to the peer.
 *   - addr: address bytes, big-endian (network order), left-justified
 *     for the active family (IPv4 = addr[0..3]). */
typedef struct {
	uint8_t  family;
	uint8_t  reserved;
	uint16_t port;
	uint8_t  addr[16];
} alp_cc3501e_sock_addr_t;

/** Payload of CMD_SOCK_OPEN (opcode 0x20).  Allocates a socket in the
 *  firmware IP stack and returns a handle the host uses on every later
 *  socket command.  The reply DATA is a single @ref
 *  alp_cc3501e_sock_handle_t (the status byte precedes it per the frame
 *  contract).
 *  Field-level meanings:
 *   - family: one of @ref alp_cc3501e_sock_family_t.
 *   - type: one of @ref alp_cc3501e_sock_type_t.
 *   - protocol: IP protocol number (0 = default for the type:
 *     TCP for STREAM, UDP for DGRAM). */
typedef struct {
	uint8_t family;
	uint8_t type;
	uint8_t protocol;
	uint8_t reserved;
} alp_cc3501e_sock_open_t;

/** Socket handle returned by CMD_SOCK_OPEN and supplied by the host on
 *  every later socket command.  The firmware treats 0 as the invalid /
 *  unallocated handle; a successful open returns a non-zero value.
 *  Field-level meanings:
 *   - handle: opaque firmware-side socket id. */
typedef struct {
	uint16_t handle;
	uint8_t  reserved[2];
} alp_cc3501e_sock_handle_t;

/** Payload of CMD_SOCK_CONNECT (opcode 0x21).  For STREAM sockets this
 *  starts the TCP handshake to @c peer; for DGRAM sockets it sets the
 *  default peer used by later parameterless CMD_SOCK_SEND.  The reply
 *  carries only the status byte.
 *  Field-level meanings:
 *   - handle: socket from CMD_SOCK_OPEN.
 *   - peer: destination endpoint, @ref alp_cc3501e_sock_addr_t. */
typedef struct {
	uint16_t                handle;
	uint16_t                reserved;
	alp_cc3501e_sock_addr_t peer;
} alp_cc3501e_sock_connect_t;

/** Payload of CMD_SOCK_SEND (opcode 0x22).  The data bytes follow this
 *  header packed inline (no padding).  data_len upper-bounds the frame
 *  length (header + data_len still <= MAX_PAYLOAD).  The reply DATA is a
 *  single uint16_t LE byte count actually queued by the firmware.
 *  Field-level meanings:
 *   - handle: socket from CMD_SOCK_OPEN.
 *   - flags: send flags (bit 0 = MORE; further bits reserved 0).
 *   - data_len: number of payload bytes that follow inline. */
typedef struct {
	uint16_t handle;
	uint8_t  flags;
	uint8_t  reserved;
	uint16_t data_len;
	uint16_t reserved2;
	/* uint8_t data[data_len];   -- packed inline, no padding */
} alp_cc3501e_sock_send_t;

/** Payload of CMD_SOCK_RECV (opcode 0x23).  Requests up to max_len
 *  bytes from the socket's receive queue.  The reply DATA is an
 *  @ref alp_cc3501e_sock_recv_resp_t followed inline by the received
 *  bytes (for DGRAM sockets the from-address is filled; for STREAM it
 *  is zeroed).
 *  Field-level meanings:
 *   - handle: socket from CMD_SOCK_OPEN.
 *   - max_len: host receive-buffer capacity for this request. */
typedef struct {
	uint16_t handle;
	uint16_t max_len;
} alp_cc3501e_sock_recv_t;

/** Reply DATA header for CMD_SOCK_RECV (precedes the received bytes,
 *  which follow inline with no padding).  data_len == 0 means no data
 *  was available (non-blocking semantics; the status is still
 *  ALP_CC3501E_RESP_OK).
 *  Field-level meanings:
 *   - from: peer endpoint for DGRAM sockets (@ref
 *     alp_cc3501e_sock_addr_t); zeroed for STREAM sockets.
 *   - data_len: number of received bytes that follow inline. */
typedef struct {
	alp_cc3501e_sock_addr_t from;
	uint16_t                data_len;
	uint16_t                reserved;
	/* uint8_t data[data_len];   -- packed inline, no padding */
} alp_cc3501e_sock_recv_resp_t;

/** Payload of CMD_SOCK_CLOSE (opcode 0x24).  Releases the firmware-side
 *  socket and (for STREAM) issues the TCP teardown.  After close the
 *  handle is invalid and the firmware may reuse its value.  The reply
 *  carries only the status byte.
 *  Field-level meanings:
 *   - handle: socket from CMD_SOCK_OPEN. */
typedef struct {
	uint16_t handle;
	uint16_t reserved;
} alp_cc3501e_sock_close_t;

/* ------------------------------------------------------------------ */
/* BLE advertising / scanning payload formats                          */
/* ------------------------------------------------------------------ */

typedef struct {
	uint8_t  connectable;
	uint8_t  reserved;
	uint16_t interval_min_ms;
	uint16_t interval_max_ms;
	uint8_t  adv_data_len;
	/* uint8_t adv_data[adv_data_len]; */
} alp_cc3501e_ble_adv_start_t;

typedef struct {
	uint8_t addr_type;
	uint8_t addr[6];
	int8_t  rssi_dbm;
	uint8_t adv_type;
	uint8_t adv_data_len;
	/* uint8_t adv_data[adv_data_len]; */
} alp_cc3501e_ble_adv_report_t;

/* ------------------------------------------------------------------ */
/* GPIO proxy payload formats                                          */
/* ------------------------------------------------------------------ */

/** Direction selector for @ref alp_cc3501e_gpio_configure_t::direction.
 *  Stored on the wire as a single byte; the named values keep callers
 *  from shipping magic numbers.  OPEN_DRAIN is required by the M.2
 *  W_DISABLE1 / W_DISABLE2 contract (host drives low to disable; HiZ
 *  releases via the board's external pull-up). */
typedef enum {
	ALP_CC3501E_GPIO_DIR_INPUT      = 0u,
	ALP_CC3501E_GPIO_DIR_OUTPUT     = 1u,
	ALP_CC3501E_GPIO_DIR_OPEN_DRAIN = 2u,
} alp_cc3501e_gpio_direction_t;

/** Internal-pull selector for @ref alp_cc3501e_gpio_configure_t::pull.
 *  Stored on the wire as a single byte.  Boards that need a stronger
 *  pull MUST add an external resistor; the on-die pull strengths are
 *  documented as weak. */
typedef enum {
	ALP_CC3501E_GPIO_PULL_NONE = 0u,
	ALP_CC3501E_GPIO_PULL_UP   = 1u,
	ALP_CC3501E_GPIO_PULL_DOWN = 2u,
} alp_cc3501e_gpio_pull_t;

typedef struct {
	uint8_t cc3501e_gpio; /**< CC3501E pad index (e.g. 13 for GPIO13). */
	uint8_t direction;    /**< One of @ref alp_cc3501e_gpio_direction_t. */
	uint8_t pull;         /**< One of @ref alp_cc3501e_gpio_pull_t. */
	uint8_t reserved;
} alp_cc3501e_gpio_configure_t;

typedef struct {
	uint8_t cc3501e_gpio;
	uint8_t level; /**< 0 or 1 */
	uint8_t reserved[2];
} alp_cc3501e_gpio_write_t;

/** Edge selector for @ref alp_cc3501e_gpio_set_interrupt_t::edge.
 *  Mirrors the firmware-side GPIO controller's edge-trigger mode
 *  registers; named here so callers don't ship magic constants.
 *  NONE doubles as "disable the IRQ" by entering the same code
 *  path on the firmware side. */
typedef enum {
	ALP_CC3501E_GPIO_EDGE_NONE    = 0u,
	ALP_CC3501E_GPIO_EDGE_RISING  = 1u,
	ALP_CC3501E_GPIO_EDGE_FALLING = 2u,
	ALP_CC3501E_GPIO_EDGE_BOTH    = 3u,
} alp_cc3501e_gpio_edge_t;

/** Payload of CMD_GPIO_SET_INTERRUPT.  Enable / disable an
 *  edge-triggered interrupt on a CC3501E GPIO and dictate which
 *  edge polarity fires the event.  After setup, the firmware
 *  emits an async ALP_CC3501E_EVT_GPIO_INTERRUPT frame on each
 *  matching edge until the host disables (edge = NONE).
 *  Field-level meanings:
 *   - cc3501e_gpio: CC3501E pad index.
 *   - edge: one of @ref alp_cc3501e_gpio_edge_t.
 *   - enabled: 0 = disable; 1 = enable. */
typedef struct {
	uint8_t cc3501e_gpio;
	uint8_t edge;
	uint8_t enabled;
	uint8_t reserved;
} alp_cc3501e_gpio_set_interrupt_t;

/** Async event payload for EVT_GPIO_INTERRUPT.  Slave -> master
 *  on every matching edge while the IRQ is enabled.  The timestamp
 *  is the CC3501E firmware's monotonic uptime counter in
 *  microseconds; host code uses it to dedupe / debounce across
 *  SPI poll cycles.
 *  Field-level meanings:
 *   - cc3501e_gpio: pad that triggered.
 *   - level: sampled level on the triggering edge.
 *   - timestamp_us: CC3501E uptime at the edge. */
typedef struct {
	uint8_t  cc3501e_gpio;
	uint8_t  level;
	uint8_t  reserved[2];
	uint32_t timestamp_us;
} alp_cc3501e_gpio_event_t;

/* ---- OTA firmware update (over-the-bridge PSA-FWU streaming) ---------------- */

/** Largest image-chunk byte count CMD_OTA_WRITE can carry: the wire payload is
 *  a 4-byte LE offset followed by the raw bytes, bounded by the frame ceiling. */
#define ALP_CC3501E_OTA_MAX_CHUNK (ALP_CC3501E_MAX_PAYLOAD - 4u)

/** Payload of CMD_OTA_BEGIN: open an OTA session.  @ref total_len is the full
 *  signed GPE vendor-image size (manifest + body) the host will stream.  The
 *  firmware picks the non-primary vendor slot and brings it to READY. */
typedef struct {
	uint32_t total_len;
} alp_cc3501e_ota_begin_t;

/** Header of CMD_OTA_WRITE: a SEQUENTIAL image chunk.  @ref offset is the
 *  absolute byte offset into the image and MUST equal the firmware's running
 *  write cursor (out-of-order writes are rejected).  The chunk bytes follow
 *  inline on the wire (length = payload_len - 4, <= ALP_CC3501E_OTA_MAX_CHUNK).
 *  The firmware buffers the first TI_FWU_MANIFEST_SIZE bytes for psa_fwu_start,
 *  then psa_fwu_write()s the remainder. */
typedef struct {
	uint32_t offset;
	/* uint8_t data[] follows (payload_len - 4 bytes). */
} alp_cc3501e_ota_write_t;

/** OTA session state, reported in @ref alp_cc3501e_ota_status_t::state. */
typedef enum {
	ALP_CC3501E_OTA_STATE_IDLE    = 0u, /**< no session open. */
	ALP_CC3501E_OTA_STATE_WRITING = 1u, /**< BEGIN done; streaming chunks. */
	ALP_CC3501E_OTA_STATE_STAGED  = 2u, /**< FINISH done; reboot-to-swap pending. */
	ALP_CC3501E_OTA_STATE_ERROR   = 3u, /**< a step failed; ABORT to reset. */
} alp_cc3501e_ota_state_t;

/** Reply payload of CMD_OTA_STATUS: lets the host resume / verify progress. */
typedef struct {
	uint8_t  state; /**< @ref alp_cc3501e_ota_state_t. */
	uint8_t  reserved[3];
	uint32_t bytes_written; /**< bytes accepted into the slot so far. */
	uint32_t total_len;     /**< total declared at BEGIN. */
} alp_cc3501e_ota_status_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_PROTOCOL_CC3501E_H */
