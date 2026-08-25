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

/* v5 adds OTA_UPDATE_MODE (0x47) -- ask the device to reboot into (or out of) the
 * polled OTA update mode.  Additive in exactly the shape v4 was (v4 exists because
 * OTA_PROMOTE 0x46 was added): a v4 host never sends it; v4 firmware rejects it
 * with RESP_ERR_INVALID, so the bump is a capability signal, not a break.
 *
 * The bump IS required here.  The counter-precedent -- OTA_STATUS reserved[1]
 * deliberately not bumping -- applies only to reusing a byte that already rode the
 * wire; a new opcode is not that.  Note cc3501e_core.c's version gate fails on ANY
 * difference and permanently clears ctx->initialised, so header, firmware and host
 * driver ship together: a bench unit still on v4 firmware must be reflashed before
 * a v5 host driver touches it.
 *
 * v5 ALSO carries ALP_CC3501E_MAX_PAYLOAD 512 -> 4096 (below).  That went through
 * an intermediate 2048 while this work was in progress, which briefly numbered
 * itself v6 and then v7 -- but v4 is the last RELEASED version (origin/dev and
 * origin/main both define 4), so every change since is unreleased and collapses
 * into ONE bump.  Do not re-derive a v6/v7 from the branch history: the wire
 * contract customers will first see after v4 is this one, and it is v5. */
#define ALP_CC3501E_PROTOCOL_VERSION 5

/** Frame header in bytes, before the payload. */
#define ALP_CC3501E_HEADER_BYTES 4

/** Maximum payload size per frame.  Larger transactions must split
 *  across multiple frames using the FRAME_CONTINUATION flag (bit 2,
 *  reserved in v1; v2 will land alongside the BLE long-write path). */
/* v5 raised this from 512.  It is NOT a wire field, so a host and firmware that
 * disagree would silently size their frame buffers differently and corrupt the
 * link -- hence the PROTOCOL_VERSION bump above, which turns the mismatch into
 * a clean GET_VERSION refusal.
 *
 * Anything raising it further must keep CONFIG_SPI_DW_ALIF_DMA_MIN_LEN ABOVE it
 * (see the example prj.conf -- DMA on the payload phase wedges the CC3501E hard
 * enough that the wedge survives host reboots), size the host's main stack for it
 * (cc3501e_sock_recv keeps a MAX_PAYLOAD reply buffer on the STACK, so 2048 ->
 * 4096 overflowed CONFIG_MAIN_STACK_SIZE 16384 outright), and fit the CC3501E
 * DRAM budget.  8192 does NOT fit: it overflows GROUP_9 by 26153 bytes and would
 * need ~26 KB relocated out of the DMA-reachable DRAM bank. */
#define ALP_CC3501E_MAX_PAYLOAD 4096

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
 *   0x30..0x3F  BLE
 *   0x40..0x4F  OTA
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
	/* Extended diagnostics.  Reply payload is
     * @ref alp_cc3501e_diag_info_t.  Adds firmware-side context
     * (reset cause, current role, uptime, free heap, last error)
     * beyond what GET_VERSION returns.  v2-firmware-only; v1
     * firmware rejects with ALP_CC3501E_RESP_ERR_INVALID. */
	ALP_CC3501E_CMD_GET_DIAG_INFO = 0x04,
	/* Async-event queue drain (host-POLLED).  The reply DATA is a packed list
	 * of queued async events, each @ref alp_cc3501e_event_entry_t
	 * { evt_opcode(1) | len(1) | payload[len] }; an empty list (data_len == 0,
	 * status OK) means nothing was queued.  The firmware DRAINS the queue on
	 * each poll so an event is delivered exactly once.
	 *
	 * WHY POLLED: the async EVT_* frames (WIFI 0x18..0x1A, BLE 0x3C..0x3F,
	 * GPIO 0x54) have no slave->master attention line on this HW rev -- the
	 * CC35 GPIO17 -> Alif P2_6 line is a BODGE not routed on the stock EVK --
	 * so the host cannot be interrupt-notified; it polls this opcode instead
	 * (a bodged unit can drive P2_6 to trigger the poll early, but the polled
	 * path is the benchable default).  0x05 is the first free opcode in the
	 * meta group (0x00..0x0F; 0x00..0x04 are taken above). */
	ALP_CC3501E_CMD_GET_PENDING_EVENTS = 0x05,

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
	/* Non-blocking connection-state poll.  The async connect model: the host
	 * SUBMITS a CMD_WIFI_CONNECT_STA (one frame, returns at once) then polls this
	 * to collect the outcome off a firmware latch -- reply alp_cc3501e_wifi_status_t. */
	ALP_CC3501E_CMD_WIFI_STATUS = 0x1B,

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
	/* 0x45 is STREAM_WRITE (below), so OTA_PROMOTE takes the next free code.
	 * Requests the swap-reboot for an image ALREADY committed to STAGED (e.g.
	 * one left pending by a bare reset that carried no swap request) -- the
	 * unjam/promote path FINISH cannot re-reach once a slot is occupied. */
	ALP_CC3501E_CMD_OTA_PROMOTE = 0x46, /* no payload; request swap of a pending image */

	/* Enter / leave OTA UPDATE MODE.  req = mode(1) { 0 = the normal DMA/callback
	 * bridge, 1 = the polled update mode }; reply = @ref alp_cc3501e_ota_update_mode_t.
	 * 0x47 is the next free code in the OTA group (0x45 is STREAM_WRITE, 0x46 is
	 * OTA_PROMOTE), is below ALP_CC3501E_CMD_RESERVED_VENDOR_BASE and can never
	 * alias ALP_CC3501E_SYNC_IDLE.
	 *
	 * WHY IT EXISTS (E1M-AEN801 silicon, 2026-08-21): a SPI_MODE_CALLBACK (DMA)
	 * SPI_open on the CC35 bridge slave PERMANENTLY prevents psa_fwu_start() and
	 * psa_fwu_write() from returning.  SPI_close() does not undo the claim and
	 * SPI_transferCancel() hangs the bridge.  So the device instead persists a flag
	 * and WARM-REBOOTS; on that boot it opens the bridge SPI polled
	 * (SPI_MODE_BLOCKING) and runs a loop that does nothing but service one frame at
	 * a time and pump the OTA flush at frame boundaries.
	 *
	 * Send this BEFORE OTA_BEGIN.  The OTA session is RAM-only, so entering
	 * mid-session throws away the write cursor and costs another full slot erase.
	 * After a successful OTA_FINISH the device leaves update mode BY ITSELF, so the
	 * swapped image comes up on the normal DMA bridge.  A cold WIFI_EN/nRESET cycle
	 * always lands in normal mode -- that asymmetry is the only escape hatch.
	 *
	 * RESP_OK means QUEUED, not "update mode is live" -- the same property OTA_BEGIN
	 * has.  A mode CHANGE reboots the device; a no-op mode request does not, and the
	 * reply's mode byte is how the host tells those two apart. */
	ALP_CC3501E_CMD_OTA_UPDATE_MODE = 0x47, /* req mode(1); reply update_mode_t */

	/* Bulk-data stream sink (proto v2).  The host sends up to
	 * ALP_CC3501E_MAX_PAYLOAD-header bytes per frame; the firmware receives +
	 * discards them (counting the total, reported via GET_DIAG_INFO) and acks.
	 * A back-to-back sequence of these is a FRAMED bulk stream: each frame's
	 * payload phase rides the host's DMA path (>= the SPI DMA threshold), and
	 * every frame is acked so the link never desyncs -- unlike clocking raw
	 * throwaway bytes.  Empty reply data. */
	ALP_CC3501E_CMD_STREAM_WRITE = 0x45, /* req: opaque bulk bytes; reply: none */

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
	/* System-wide power policy for the CC3501E itself.
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
	/** Op rejected because of the subsystem's CURRENT state (e.g. NimBLE's
	 *  ble_gatts_mutable() ordering guard on BLE_GATT_REGISTER while
	 *  advertising/scanning/connected).  Distinct from
	 *  @ref ALP_CC3501E_RESP_ERR_RADIO -- this is a deterministic, terminal reject:
	 *  retrying without the caller first changing that state (stop
	 *  advertising / disconnect) repeats the same answer, so the host must
	 *  not poll-by-repeat it like a transient radio/bridge fault. */
	ALP_CC3501E_RESP_ERR_STATE    = 0x09,
	ALP_CC3501E_RESP_ERR_INTERNAL = 0xFF
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
 *   - role: one of @ref alp_cc3501e_role_t.  WI-FI ONLY on current
 *     firmware -- ROLE_OFF, ROLE_WIFI_STA or ROLE_WIFI_AP.  BLE state is
 *     not folded in, so ROLE_OFF here does NOT mean BLE is down.  Until
 *     alp-sdk#1562 this byte was hardcoded ROLE_OFF and told you nothing.
 *   - uptime_ms: time since power-on / last reset.
 *   - free_heap_bytes: firmware-allocator free pool.  Between #1553 and
 *     #1562 the firmware shipped the last Wi-Fi event ID in this field
 *     instead, so a host reading it as heap saw a small integer (often 0)
 *     and rendered an alarming "0 B free"; the event ID now lives in
 *     reserved[0] and this field means what it says again.
 *   - last_error: last @ref alp_cc3501e_resp_t the firmware
 *     emitted on the wire; @ref ALP_CC3501E_RESP_OK if no error
 *     since last reset.
 *   - reserved[0]: low byte of the last Wi-Fi event ID the firmware's
 *     event callback saw; 0 = none since reset (also what pre-#1562
 *     firmware put here, so an old host reading 0 is not misled).  An
 *     ap_start that leaves this at 0 never received a WLAN event at all.
 *   - reserved[1..2]: still reserved, always 0. */
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
/* Async-event queue payload format (CMD_GET_PENDING_EVENTS)           */
/* ------------------------------------------------------------------ */

/** Fixed per-event header size on the wire (evt_opcode + len). */
#define ALP_CC3501E_EVENT_HDR_BYTES 2u

/** One entry in a CMD_GET_PENDING_EVENTS (0x05) reply.  The reply DATA (the
 *  bytes after the frame's status byte) is a packed list of ZERO OR MORE
 *  entries, each laid out on the wire with NO padding as:
 *
 *    offset 0            evt_opcode (1)   -- one of the ALP_CC3501E_EVT_* opcodes
 *    offset 1            len        (1)   -- payload byte count (0..255)
 *    offset 2            payload[len]     -- event-specific bytes (the matching
 *                                            EVT_* struct; empty for the Wi-Fi
 *                                            connect/disconnect events)
 *
 *  The next entry begins immediately at offset (2 + len); the list ends when
 *  the reply DATA is exhausted.  The firmware packs WHOLE entries only -- an
 *  event whose payload would overflow the reply is held back for the next poll,
 *  never split -- and DRAINS each entry it emits, so every event is delivered
 *  exactly once.  Field-level meanings:
 *   - evt_opcode: the async opcode (e.g. @ref ALP_CC3501E_EVT_WIFI_CONNECTED).
 *   - len: number of payload bytes that follow inline. */
typedef struct {
	uint8_t evt_opcode;
	uint8_t len;
	/* uint8_t payload[len];   -- packed inline, no padding */
} alp_cc3501e_event_entry_t;

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

/** Connection state reported by CMD_WIFI_STATUS (opcode 0x1B).  The async connect
 *  model: the host SUBMITS a CMD_WIFI_CONNECT_STA (one frame, returns at once) and
 *  then polls this NON-BLOCKING status to learn the outcome -- CONNECTING while the
 *  association runs (the bridge READY line is held BUSY then), CONNECTED or FAILED
 *  once the firmware's WLAN connect event lands.  CONNECTED / FAILED are TERMINAL
 *  (the host reads them once + stops); a fresh CMD_WIFI_CONNECT_STA starts a new
 *  attempt (the firmware re-arms the latch to CONNECTING on submit). */
typedef enum {
	ALP_CC3501E_WIFI_DISCONNECTED = 0u, /**< no association + none in flight. */
	ALP_CC3501E_WIFI_CONNECTING   = 1u, /**< association in progress (bridge BUSY). */
	ALP_CC3501E_WIFI_CONNECTED    = 2u, /**< associated (see rssi_dbm's warning). */
	ALP_CC3501E_WIFI_CONN_FAILED  = 3u, /**< attempt failed (fail_reason valid). */
} alp_cc3501e_wifi_conn_state_t;

/** Failure detail in @ref alp_cc3501e_wifi_status_t::fail_reason (meaningful only
 *  when state == @ref ALP_CC3501E_WIFI_CONN_FAILED). */
typedef enum {
	ALP_CC3501E_WIFI_FAIL_NONE     = 0u, /**< not a failure state. */
	ALP_CC3501E_WIFI_FAIL_TIMEOUT  = 1u, /**< no WLAN connect event within the wait. */
	ALP_CC3501E_WIFI_FAIL_REJECTED = 2u, /**< association / authentication rejected. */
	ALP_CC3501E_WIFI_FAIL_KICK     = 3u, /**< STA role-up / Wlan_Connect kick failed. */
} alp_cc3501e_wifi_fail_t;

/** Reply payload of CMD_WIFI_STATUS (opcode 0x1B): a NON-BLOCKING snapshot of the
 *  STA connection state, read off a firmware latch (no radio op, ISR-safe) -- how
 *  the host collects an async connect result without blocking.  Fixed 4-byte wire
 *  layout (no padding): state | fail_reason | rssi_dbm | reserved. */
typedef struct {
	uint8_t state;       /**< @ref alp_cc3501e_wifi_conn_state_t. */
	uint8_t fail_reason; /**< @ref alp_cc3501e_wifi_fail_t (when state == FAILED). */
	/** @warning NOT A MEASUREMENT as of protocol v4 -- always 0 on the wire.
	 *  The firmware latch this byte is served from is never populated: every
	 *  terminal outcome publishes it via wifi_conn_set(), which always sets it
	 *  to 0 (the connect body may not read the RSSI near associate -- the read
	 *  blocks that worker), so the byte has only ever held 0.  0 dBm is a LEGAL
	 *  int8 RSSI, so there is no in-band sentinel a reader can test to tell
	 *  "unmeasured" from a real 0 -- do NOT report this byte as a signal level
	 *  (issue #1387).  A real reading comes only from CMD_WIFI_GET_RSSI (0x16),
	 *  a worker-routed radio read.  Populating this byte honestly needs either a
	 *  bench answer on whether the post-DHCP read is safe, or a validity flag on
	 *  the wire (the @c reserved byte) -- both open; neither is decided here. */
	int8_t  rssi_dbm;
	uint8_t reserved;
} alp_cc3501e_wifi_status_t;

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
/* BLE_GATT_REGISTER (0x38) dynamic-service payload format             */
/* ------------------------------------------------------------------ */

/** Max characteristics accepted per BLE_GATT_REGISTER call.  Bounds the
 *  worst-case reply (one uint16 handle per characteristic) and the firmware's
 *  fixed-size attribute-table scratch storage -- 8 is generous for a v1
 *  sensor/actuator service and keeps both comfortably inside
 *  ALP_CC3501E_MAX_PAYLOAD. */
#define ALP_CC3501E_BLE_GATT_MAX_CHARS 8u

/** BLE_GATT_REGISTER wire version (the request's first byte).  Bump this if
 *  the layout below changes in a way older firmware/hosts can't parse. */
#define ALP_CC3501E_BLE_GATT_REGISTER_VERSION 1u

/**
 * @brief BLE_GATT_REGISTER (0x38) request/reply wire format -- one service
 *        per call, host (Alif) -> firmware (CC3501E).
 *
 * All multi-byte fields little-endian.  This is a VARIABLE-length payload
 * (each characteristic carries its own initial_value span), so -- like the
 * other BLE payloads in this header -- it is parsed field-by-field on both
 * sides rather than cast onto a single struct; @ref
 * alp_cc3501e_ble_gatt_register_hdr_t names the fixed leading fields both
 * sides share, but the per-characteristic records are documented here only
 * (a struct would need a uint16 field at an odd offset -- see the
 * PACKED-wire note at the top of this file).
 *
 * REQUEST (host -> firmware), total length must equal req_len exactly (no
 * trailing bytes):
 * @code
 *   u8      version                     ALP_CC3501E_BLE_GATT_REGISTER_VERSION
 *   u8[16]  service_uuid                verbatim alp_ble_uuid_t.b -- NOT byte-swapped
 *   u8      num_chars                   1 .. ALP_CC3501E_BLE_GATT_MAX_CHARS
 *   repeat num_chars:
 *     u8[16] char_uuid                  verbatim alp_ble_uuid_t.b
 *     u8     properties                 ALP_BLE_GATT_PROP_* (== BT-SIG / NimBLE
 *                                        BLE_GATT_CHR_F_* bits -- READ 0x02,
 *                                        WRITE 0x08, NOTIFY 0x10, INDICATE 0x20)
 *     u16    initial_len (LE)
 *     u8[initial_len] initial_value
 * @endcode
 *
 * REPLY (firmware -> host):
 * @code
 *   u8   status         0 = OK, nonzero = firmware error
 *   u8   num_handles    == num_chars on success, 0 on error
 *   repeat num_handles: u16 attr_handle (LE) -- the characteristic VALUE handle
 * @endcode
 *
 * The frame-level alp_cc3501e_resp_t (the response status byte in the
 * transport header) is authoritative for whether the call succeeded; this
 * in-payload `status` is carried so the reply is self-describing on its own
 * -- today's firmware only emits this payload at all on the OK path, so it
 * is always 0 when present.
 *
 * NimBLE lifecycle caveat (see cc3501e_nimble_gatt_register() for the full
 * rationale): a service registered while the NimBLE host is already up
 * requires the firmware to re-run ble_gatts_start() -- the vendor SDK's own
 * documented (not invented) way to add services after the first one, but
 * unverified on silicon as of this wire-format revision.  A firmware error
 * reply here means the service did NOT take effect; retry after a fresh
 * BLE_ENABLE if the error persists.
 *
 * The frame-level resp is @ref ALP_CC3501E_RESP_ERR_STATE (not
 * ALP_CC3501E_RESP_ERR_RADIO) when ble_gatts_start()/add_svcs()/reset() hit
 * NimBLE's ble_gatts_mutable() ordering guard (BLE_HS_EBUSY -- an active
 * connection/adv/discover/connect) -- the host maps that to ALP_ERR_BUSY and
 * does not retry it; a genuine transport/radio failure still surfaces as
 * ALP_CC3501E_RESP_ERR_RADIO -> ALP_ERR_IO.
 */
typedef struct {
	uint8_t version;
	uint8_t service_uuid[16];
	uint8_t num_chars;
	/* per-characteristic records follow -- see the wire format above;
	 * NOT modeled as a struct array here (uint16 initial_len would land at
	 * an odd offset and pick up compiler padding on some ABIs). */
} alp_cc3501e_ble_gatt_register_hdr_t;

/** Reply header for BLE_GATT_REGISTER; @c attr_handle[num_handles] (LE16
 *  each) follows inline -- see the wire format doc above. */
typedef struct {
	uint8_t status;
	uint8_t num_handles;
} alp_cc3501e_ble_gatt_register_reply_hdr_t;

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

/* ------------------------------------------------------------------ */
/* Wire-layout guards (#733)                                          */
/*                                                                    */
/* Two categories of struct live in this header:                     */
/*                                                                    */
/*  (A) DIRECTLY SERIALIZED -- the host builds the struct and hands   */
/*      its raw address to the SPI DMA, and the firmware casts the    */
/*      received wire buffer straight back to the struct type.  Its   */
/*      in-memory layout IS the wire format, so ANY tail/interior     */
/*      padding silently corrupts the frame.  These are padding-free  */
/*      TODAY only because every field is uint8_t; adding a wider     */
/*      field would reintroduce the alp_cc3501e_ble_adv_start_t       */
/*      7-vs-8 gap.  The asserts below make that a BUILD failure      */
/*      instead of a field-shift on the wire -- keep these all-       */
/*      uint8_t, or switch the op to a hand-packed codec before       */
/*      widening a field.                                             */
/*                                                                    */
/*  (B) WIRE-SCHEMA -- documentary layout only.  Both sides encode/   */
/*      decode these field-by-field with explicit little-endian byte  */
/*      access (never memcpy / pointer-cast), so compiler padding is  */
/*      irrelevant to the wire.  Where the firmware still uses        */
/*      sizeof(T) as the expected payload length (the socket ops),    */
/*      the size is pinned below so a field reorder can't drift that  */
/*      length constant out from under the byte-offset parser.        */
/* ------------------------------------------------------------------ */

/* (A) Directly-serialized command payloads -- struct-punned on both   */
/*     sides (chips/cc3501e/cc3501e.c hands &struct to the SPI DMA;    */
/*     firmware/cc3501e/src/protocol.c casts the wire buffer to T*).   */
_Static_assert(sizeof(alp_cc3501e_gpio_configure_t) == 4u,
               "gpio_configure wire header must stay 4 bytes / padding-free");
_Static_assert(offsetof(alp_cc3501e_gpio_configure_t, cc3501e_gpio) == 0u, "gpio_configure @0");
_Static_assert(offsetof(alp_cc3501e_gpio_configure_t, direction) == 1u,
               "gpio_configure.direction @1");
_Static_assert(offsetof(alp_cc3501e_gpio_configure_t, pull) == 2u, "gpio_configure.pull @2");

_Static_assert(sizeof(alp_cc3501e_gpio_write_t) == 4u,
               "gpio_write wire header must stay 4 bytes / padding-free");
_Static_assert(offsetof(alp_cc3501e_gpio_write_t, cc3501e_gpio) == 0u, "gpio_write @0");
_Static_assert(offsetof(alp_cc3501e_gpio_write_t, level) == 1u, "gpio_write.level @1");

_Static_assert(sizeof(alp_cc3501e_gpio_set_interrupt_t) == 4u,
               "gpio_set_interrupt wire header must stay 4 bytes / padding-free");
_Static_assert(offsetof(alp_cc3501e_gpio_set_interrupt_t, cc3501e_gpio) == 0u, "gpio_irq @0");
_Static_assert(offsetof(alp_cc3501e_gpio_set_interrupt_t, edge) == 1u, "gpio_irq.edge @1");
_Static_assert(offsetof(alp_cc3501e_gpio_set_interrupt_t, enabled) == 2u, "gpio_irq.enabled @2");

_Static_assert(sizeof(alp_cc3501e_wifi_connect_t) == 4u,
               "wifi_connect wire header must stay 4 bytes / padding-free");
_Static_assert(offsetof(alp_cc3501e_wifi_connect_t, ssid_len) == 0u, "wifi_connect.ssid_len @0");
_Static_assert(offsetof(alp_cc3501e_wifi_connect_t, psk_len) == 1u, "wifi_connect.psk_len @1");
_Static_assert(offsetof(alp_cc3501e_wifi_connect_t, security) == 2u, "wifi_connect.security @2");

/* gpio_event is wire-shaped (async EVT_GPIO_INTERRUPT payload) and has a
 * uint32_t, so alignment -- not just tail padding -- pins its layout.
 * End-to-end async delivery is not wired yet (#130); guard it now so the
 * layout is locked when it is. */
_Static_assert(sizeof(alp_cc3501e_gpio_event_t) == 8u, "gpio_event payload must stay 8 bytes");
_Static_assert(offsetof(alp_cc3501e_gpio_event_t, timestamp_us) == 4u,
               "gpio_event.timestamp_us @4");

/* (B) Schema structs whose sizeof() is the firmware's expected payload
 *     length for the socket ops (protocol.c handle_sock_*).  Field access
 *     is byte-offset on both sides; pin the size so a reorder can't move
 *     the length constant without failing the build. */
_Static_assert(sizeof(alp_cc3501e_sock_open_t) == 4u, "sock_open wire length");
_Static_assert(sizeof(alp_cc3501e_sock_handle_t) == 4u, "sock_handle wire length");
_Static_assert(sizeof(alp_cc3501e_sock_connect_t) == 24u, "sock_connect wire length");
_Static_assert(sizeof(alp_cc3501e_sock_send_t) == 8u, "sock_send wire header length");
_Static_assert(sizeof(alp_cc3501e_sock_recv_t) == 4u, "sock_recv wire length");
_Static_assert(sizeof(alp_cc3501e_sock_recv_resp_t) == 24u, "sock_recv_resp wire header length");
_Static_assert(sizeof(alp_cc3501e_sock_close_t) == 4u, "sock_close wire length");
_Static_assert(sizeof(alp_cc3501e_sock_addr_t) == 20u, "sock_addr wire length");

/* The canonical 7-byte-wire / 8-byte-struct case the issue calls out
 * (#733): alp_cc3501e_ble_adv_start_t is hand-packed to 7 bytes on both
 * sides PRECISELY because its C sizeof is 8 (tail pad after adv_data_len).
 * Assert the 8 so nobody "optimises" the hand-packing into a memcpy that
 * would ship the pad byte. */
_Static_assert(sizeof(alp_cc3501e_ble_adv_start_t) == 8u,
               "ble_adv_start struct is 8 bytes; wire header is hand-packed to 7 -- do NOT memcpy");

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
	uint8_t state; /**< @ref alp_cc3501e_ota_state_t. */
	/** reserved[0] = last swap-reboot rc: 0 = none requested / success (the device
	 *  reboots on success and never reports it), non-zero = the swap was REFUSED
	 *  (e.g. BL2 anti-rollback on a downgrade).
	 *
	 *  reserved[1] = FLUSH PENDING (#1610), and reading it is MANDATORY for any
	 *  host that streams OTA_WRITE.  Non-zero means the device has queued a
	 *  staging-window flush to flash: it is about to tear down its bridge DMA and
	 *  will not consume payload until the flag clears.  A host that keeps clocking
	 *  OTA_WRITE across that window desyncs the link permanently.  The contract is
	 *  to hold off ALL payload and poll THIS field with header-only OTA_STATUS
	 *  frames until it reads 0, then re-send the same chunk -- and to reconcile
	 *  against @ref bytes_written first, because OTA_WRITE is not idempotent and a
	 *  chunk whose reply was swallowed by the blackout may already have landed.
	 *
	 *  reserved[2] = diagnostics, in TWO encodings:
	 *    1..0x3F  the psa_fwu_* call that failed the last window flush.
	 *    0x40|p   no psa fault; the low 6 bits are the transport PHASE, and bit
	 *    0xC0|p   0x80 set means the bridge is running POLLED.  This is the shape
	 *             a HEALTHY session reports, so do not treat a non-zero
	 *             reserved[2] as a fault on its own.
	 *    0        the device published neither. */
	uint8_t  reserved[3];
	uint32_t bytes_written; /**< bytes accepted into the slot so far. */
	uint32_t total_len;     /**< total declared at BEGIN. */
} alp_cc3501e_ota_status_t;

/** Reply payload of CMD_OTA_UPDATE_MODE: the mode the device is running RIGHT NOW
 *  (so still 0 on the ack that merely QUEUES entry -- the device has not rebooted
 *  yet).  The host confirms entry by re-issuing 0x47 until @ref mode matches what
 *  it asked for.
 *
 *  FOUR BYTES ON PURPOSE.  A dead bus phase clocks back literal 0x00 for every byte
 *  and 0x00 is also ALP_CC3501E_RESP_OK, so a status-only reply is byte-identical
 *  to a link that died in the inter-phase gap.  0x47 is the sharpest instance of
 *  that -- its whole job is to be the last frame before the link blacks out.
 *
 *  The mode byte defeats that alias, but ASYMMETRICALLY -- write the host confirm
 *  loop to the asymmetry, do not assume a general structural defeat:
 *    - mode == 1 (ENTERED update mode) IS proof.  A dead phase reads back 0x00, so
 *      it can never forge the 1 the host is waiting for.
 *    - mode == 0 (LEFT update mode) is NOT proof.  An all-zero dead phase is
 *      byte-identical to a genuine "normal bridge, OTA idle" reply, so a host
 *      polling for mode == 0 must corroborate before reporting success (a moving
 *      GET_DIAG_INFO uptime_ms, or simply the next live command).
 *
 *  Either way this reply is 5 payload bytes (status + 4), while the all-zero
 *  blacklist in chips/cc3501e/cc3501e_core.c fires only on resp_payload_len == 1 --
 *  the bare-status shape OTA_PROMOTE has and could not escape.  Listing 0x47 there
 *  would be dead code; do NOT add it. */
typedef struct {
	uint8_t mode;        /**< 0 = normal DMA bridge, 1 = polled OTA update mode. */
	uint8_t ota_state;   /**< @ref alp_cc3501e_ota_state_t, as OTA_STATUS reports it. */
	uint8_t reserved[2]; /**< MBZ; the additive-extension channel, as used twice above. */
} alp_cc3501e_ota_update_mode_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_PROTOCOL_CC3501E_H */
