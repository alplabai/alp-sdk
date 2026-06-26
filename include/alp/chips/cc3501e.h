/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file cc3501e.h
 * @brief Alif-side host driver for the on-module TI CC3501E
 *
 * @par Verification status: [UNTESTED] -- driver compiles + passes NULL-arg smokes;
 *   no HiL silicon bring-up yet.  Treat all numbers + lifecycle
 *   sequencing as paper-correct only until the v1.0 verification
 *   sweep lands.
 *        Wi-Fi 6 + BLE 5.4 coprocessor.
 *
 * Wraps the inter-chip SPI1 host-control protocol defined in
 * `<alp/protocol/cc3501e.h>` with a synchronous request/response
 * call shape plus an async-event callback.  Sits below
 * `<alp/iot.h>` and `<alp/ble.h>` -- those public surfaces
 * dispatch through this driver when the active SoM is an
 * E1M-AEN family member.
 *
 * Lifecycle:
 *
 * @code
 *   alp_spi_t *bus = alp_spi_open(&(alp_spi_config_t){
 *       .bus_id = E1M_SPI1,   // inter-chip SPI bus
 *       .freq_hz = 8000000,
 *       .mode = ALP_SPI_MODE_0,
 *       .bits_per_word = 8,
 *   });
 *   cc3501e_t fw;
 *   cc3501e_init(&fw, bus);
 *   cc3501e_reset(&fw);            // pulses E_WIFI.NRST + WIFI.EN
 *   uint16_t version = 0;
 *   cc3501e_get_version(&fw, &version);
 *   if (version != ALP_CC3501E_PROTOCOL_VERSION) { ... }
 * @endcode
 *
 * The Alif drives `WIFI.EN` (P15_5) and `E_WIFI.NRST`
 * (P15_1_FLEX) through alp_gpio_*; the driver reads those pin
 * IDs from the studio-supplied config or falls back to
 * compile-time defaults baked from
 * `metadata/e1m_modules/aen/inter-chip.tsv`.
 */

#ifndef ALP_CHIPS_CC3501E_H
#define ALP_CHIPS_CC3501E_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "alp/peripheral.h"
#include "alp/protocol/cc3501e.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct cc3501e cc3501e_t;

/** Async event callback -- runs on the driver's RX thread.
 *  @p cmd is the event opcode (one of `ALP_CC3501E_EVT_*`),
 *  @p payload + @p len carry the event-specific data described
 *  in `<alp/protocol/cc3501e.h>`, and @p user is the opaque pointer
 *  registered with @ref cc3501e_set_event_callback. */
typedef void (*cc3501e_event_cb_t)(uint8_t cmd, const uint8_t *payload, size_t len, void *user);

/** @brief Driver context for one CC3501E bridge link.
 *
 *  Caller-allocated and owned for the lifetime of the link; the fields are
 *  driver-private (treat as opaque) -- populate via @ref cc3501e_init. */
struct cc3501e {
	bool               initialised; /**< True between a successful init and deinit. */
	alp_spi_t         *bus;         /**< SPI1 to the CC3501E (Alif master). */
	alp_gpio_t        *enable_pin;  /**< WIFI.EN (P15_5).  May be NULL on boards that tie it on. */
	alp_gpio_t        *reset_pin;   /**< E_WIFI.NRST (P15_1_FLEX). */
	cc3501e_event_cb_t event_cb;    /**< Async-event callback, or NULL if none. */
	void              *event_user;  /**< Opaque pointer passed back to @c event_cb. */
	/** Reassembly buffer for an inbound frame (header + max payload). */
	uint8_t rx_scratch[ALP_CC3501E_HEADER_BYTES + ALP_CC3501E_MAX_PAYLOAD];
	/** Staging buffer for an outbound frame (header + max payload). */
	uint8_t tx_scratch[ALP_CC3501E_HEADER_BYTES + ALP_CC3501E_MAX_PAYLOAD];
};

/**
 * @brief Initialise the driver and bind it to an open SPI1 bus.
 *
 * Does not enable the radio -- call @ref cc3501e_reset to bring
 * the firmware up.  @p bus must remain valid for the lifetime
 * of @p ctx.
 *
 * @param ctx  Driver context (output).
 * @param bus  Opened inter-chip SPI1 bus (Alif master); caller retains ownership.
 * @return ALP_OK on success; ALP_ERR_INVAL on a NULL argument.
 */
alp_status_t cc3501e_init(cc3501e_t *ctx, alp_spi_t *bus);

/**
 * @brief Reset the module: pulse the reset line then de-assert WIFI.EN.
 *
 * Blocks until the firmware's PING reply arrives or an internal timeout
 * expires.  Issues the cold-boot re-boot that works around the Puya-flash
 * mis-read (see @ref cc3501e_hard_reset).
 *
 * @param ctx  Initialised driver context.
 * @return ALP_OK once the firmware answers PING; ALP_ERR_TIMEOUT if it never
 *         does; otherwise the mapped error.
 */
alp_status_t cc3501e_reset(cc3501e_t *ctx);

/**
 * @brief Warm hard reset: pulse nRESET with WIFI_EN kept asserted (rails stay up).
 *
 * Re-boots the module WITHOUT a cold power cycle.  This is the "second boot" of the
 * CC3501E Puya-flash (PY25Q64LB / 64Mbit) cold-boot workaround: a cold power-on
 * mis-reads the Puya flash on the FIRST boot (TI SDK bug, 32/64Mbit Puya parts), so
 * the secure boot never launches the vendor image; a hard reset re-boots with the
 * flash settled and the image launches.  @ref cc3501e_reset already issues one such
 * re-boot after the cold power-up; call this again (e.g. from a soak/retry loop) if
 * a single re-boot has not brought the link up.  Remove once TI ships the flash fix.
 *
 * @param ctx Initialised driver context (must have @c reset_pin populated).
 * @return ALP_OK after the re-boot budget elapses; ALP_ERR_NOSUPPORT if no reset pin.
 */
alp_status_t cc3501e_hard_reset(cc3501e_t *ctx);

/**
 * @brief (Re)establish byte alignment by walking the SPI byte phase.
 *
 * A best-effort cold first-contact probe: before the slave's SPI peripheral
 * is armed its SS0 input is not yet framing transfers, so a missed/extra
 * clock (or a slave that booted mid-transfer) can leave the two sides
 * byte-misaligned within a transfer.  This walks the SPI byte phase until it
 * observes the slave's header-idle marker (@ref ALP_CC3501E_SYNC_IDLE,
 * driven only when the slave is parked at a clean request-header boundary),
 * confirming with two consecutive aligned reads to reject a stray marker byte
 * inside reply data.  Call it before the first request after reset.  (The
 * steady-state request path does NOT byte-walk: each phase is its own
 * hardware-SS0-framed transfer, so a per-transaction SS0 edge realigns the
 * slave on the caller's next request after a desync.)
 *
 * @param ctx         Initialised driver context.
 * @param timeout_ms  Coarse upper bound on re-sync effort (each ~ms covers
 *                    one full-frame byte-walk attempt).
 * @return ALP_OK once aligned; ALP_ERR_TIMEOUT if the slave never parked
 *         (e.g. unpowered / not running its firmware).
 */
alp_status_t cc3501e_sync(cc3501e_t *ctx, uint32_t timeout_ms);

/**
 * @brief Retrieve the firmware's protocol version.
 *
 * Compare against `ALP_CC3501E_PROTOCOL_VERSION` to confirm wire compatibility.
 *
 * @param ctx          Initialised driver context.
 * @param version_out  Receives the firmware's 16-bit protocol version.
 * @return ALP_OK with @p version_out set; ALP_ERR_IO on a short reply;
 *         otherwise the mapped error.
 */
alp_status_t cc3501e_get_version(cc3501e_t *ctx, uint16_t *version_out);

/* ------------------------------------------------------------------ */
/* Wi-Fi host helpers                                                  */
/*                                                                     */
/* Thin wrappers over @ref cc3501e_request that match the opcodes +    */
/* payloads in <alp/protocol/cc3501e.h>.  Several of these poll the    */
/* firmware "by repeat": the firmware accepts the request, kicks off a */
/* worker (scan / DHCP / association), and returns ALP_ERR_BUSY        */
/* (RESP_ERR_BUSY) on each repeat until the worker finishes -- so the  */
/* host re-issues the SAME command on a bounded retry/backoff until it */
/* gets RESP_OK with the result, or the timeout elapses.  This is how  */
/* the bring-up proves the firmware's worker seam from the host with   */
/* no async-event line on this HW rev.                                 */
/* ------------------------------------------------------------------ */

/** MAC address length in bytes (the GET_MAC reply data). */
#define CC3501E_MAC_LEN 6u

/**
 * @brief Read the CC3501E's Wi-Fi station MAC address (GET_MAC, opcode 0x03).
 *
 * GET_MAC is poll-by-repeat on the firmware side: the firmware may answer
 * RESP_ERR_BUSY while the radio identity is still being read out of the
 * device, so this loops @ref cc3501e_request(GET_MAC) on a bounded backoff
 * while it returns @ref ALP_ERR_BUSY, until RESP_OK fills the 6-byte MAC or
 * @p timeout_ms elapses.  Proving this round-trips exercises the firmware
 * worker seam from the host.
 *
 * @param ctx         Initialised driver context.
 * @param mac         Receives the 6-byte MAC (@ref CC3501E_MAC_LEN) on success.
 * @param timeout_ms  Upper bound on the poll-by-repeat budget.
 * @return ALP_OK with @p mac filled; ALP_ERR_TIMEOUT if the firmware stayed
 *         busy; ALP_ERR_IO on a short reply; or the mapped error otherwise.
 *
 * @note WIRE: GET_MAC has an opcode but the protocol header defines NO reply
 *       payload struct; this helper assumes the reply data is exactly the
 *       6 MAC bytes (after the status byte).  See cc3501e.c for the gap note.
 */
alp_status_t
cc3501e_wifi_get_mac(cc3501e_t *ctx, uint8_t mac[CC3501E_MAC_LEN], uint32_t timeout_ms);

/**
 * @brief One parsed Wi-Fi scan record handed back to the caller.
 *
 * Mirrors the on-wire @ref alp_cc3501e_scan_result_t fixed header plus the
 * record's inline SSID copied out into a NUL-terminated buffer (the wire
 * SSID is length-prefixed, not NUL-terminated).  SSIDs longer than
 * @ref CC3501E_SSID_MAX are truncated.
 */
#define CC3501E_SSID_MAX 32u
typedef struct {
	uint8_t  bssid[6];                    /**< AP BSSID (MAC). */
	int8_t   rssi_dbm;                    /**< Received signal strength, dBm. */
	uint8_t  channel;                     /**< Wi-Fi channel. */
	uint16_t security_info;               /**< Raw TI scan-result SecurityInfo (16-bit, LE on
	                                        *   the wire).  Decode with @ref cc3501e_wifi_sec_kind
	                                        *   / @ref cc3501e_wifi_sec_name -- the sec-type lives
	                                        *   in the high byte, so the old 1-byte field carried
	                                        *   only the group cipher (always read "?"). */
	uint8_t  ssid_len;                    /**< SSID length as reported on the wire. */
	char     ssid[CC3501E_SSID_MAX + 1u]; /**< NUL-terminated SSID copy. */
} cc3501e_scan_record_t;

/**
 * @brief Decoded Wi-Fi security kind (from a scan record's @c security_info).
 *
 * The CC3501E scan reports the raw TI 16-bit SecurityInfo; these are the
 * human-meaningful buckets the console maps it to.  @c sec_type lives at
 * @c (security_info >> 8) & 0x3f (TI WLAN_SCAN_RESULT_SEC_TYPE_BITMAP); the SAE
 * bits (0x08|0x10) in that bitmap mark WPA3, the WPA2 bit is 0x04, open is 0.
 */
typedef enum {
	CC3501E_WIFI_SEC_OPEN    = 0,   /**< No security (open network). */
	CC3501E_WIFI_SEC_WEP     = 1,   /**< Legacy WEP. */
	CC3501E_WIFI_SEC_WPA     = 2,   /**< WPA (TKIP-era). */
	CC3501E_WIFI_SEC_WPA2    = 3,   /**< WPA2-PSK. */
	CC3501E_WIFI_SEC_WPA3    = 4,   /**< WPA3-SAE. */
	CC3501E_WIFI_SEC_UNKNOWN = 255, /**< SecurityInfo did not map to a known kind. */
} cc3501e_wifi_sec_t;

/** @brief Decode a scan record's raw @c security_info into a @ref cc3501e_wifi_sec_t. */
cc3501e_wifi_sec_t cc3501e_wifi_sec_kind(uint16_t security_info);

/** @brief Short human name ("open"/"wep"/"wpa"/"wpa2"/"wpa3"/"sec?") for @c security_info. */
const char *cc3501e_wifi_sec_name(uint16_t security_info);

/**
 * @brief Run a Wi-Fi scan and collect the results (WIFI_SCAN_START, 0x10).
 *
 * Poll-by-repeat: re-issues WIFI_SCAN_START while the firmware reports
 * RESP_ERR_BUSY (scan in progress).  Once the scan completes the firmware
 * returns RESP_OK with the discovered access points packed into the reply
 * payload as a sequence of @ref alp_cc3501e_scan_result_t records (each
 * fixed 10-byte header immediately followed by its @c ssid_len inline SSID
 * bytes, no padding).  This parses up to @p cap records out into
 * @p out_records and writes the count to @p count.
 *
 * @param ctx         Initialised driver context.
 * @param out_records Caller array of @p cap @ref cc3501e_scan_record_t.
 * @param cap         Capacity of @p out_records.
 * @param count       Receives the number of records parsed (may be NULL).
 * @param timeout_ms  Upper bound on the poll-by-repeat budget.
 * @return ALP_OK once the scan completed (even with zero records);
 *         ALP_ERR_TIMEOUT if the firmware stayed busy; mapped error otherwise.
 *
 * @note WIRE: the protocol header defines @ref alp_cc3501e_scan_result_t but
 *       NO count/list envelope and documents the records as async events
 *       (EVT_WIFI_SCAN_RESULT). This helper assumes the firmware returns the
 *       records as the SCAN_START reply payload. See cc3501e.c for the gap.
 */
alp_status_t cc3501e_wifi_scan(cc3501e_t             *ctx,
                               cc3501e_scan_record_t *out_records,
                               size_t                 cap,
                               size_t                *count,
                               uint32_t               timeout_ms);

/**
 * @brief Associate with a Wi-Fi AP (WIFI_CONNECT_STA, opcode 0x12).
 *
 * Submits the SSID / security / passphrase as the on-wire
 * @ref alp_cc3501e_wifi_connect_t header followed by the inline SSID then
 * the inline passphrase, then polls status to connected / failed: while the
 * firmware reports RESP_ERR_BUSY (association in progress) this re-issues the
 * same request on a bounded backoff until RESP_OK (connected) or the timeout.
 *
 * @param ctx         Initialised driver context.
 * @param ssid        NUL-terminated SSID (<= 32 bytes; longer is rejected).
 * @param sec_type    Security: 0 = open, 1 = WPA2-PSK, 2 = WPA3-SAE
 *                    (matches @ref alp_cc3501e_wifi_connect_t::security).
 * @param pass        NUL-terminated passphrase (may be NULL/"" for open).
 * @param timeout_ms  Upper bound on the connect poll budget.
 * @return ALP_OK once associated; ALP_ERR_TIMEOUT if still associating at
 *         the deadline; ALP_ERR_INVAL on an over-long SSID/passphrase;
 *         mapped error otherwise.
 */
alp_status_t cc3501e_wifi_connect(
    cc3501e_t *ctx, const char *ssid, uint8_t sec_type, const char *pass, uint32_t timeout_ms);

/**
 * @brief Submit a Wi-Fi association and RETURN IMMEDIATELY (async connect).
 *
 * Sends ONE CMD_WIFI_CONNECT_STA frame and returns as soon as the firmware has
 * accepted the job into its worker (the submit ack) -- it does NOT block for the
 * seconds-long association.  The firmware runs the association in the background
 * (holding the bridge READY line BUSY while the radio op runs); collect the result
 * later, non-blocking, via @ref cc3501e_wifi_status (poll it when @ref
 * cc3501e_bus_ready is true again).  This is what keeps a host shell responsive
 * during a connect.
 *
 * @param ctx       Initialised driver context.
 * @param ssid      NUL-terminated SSID (<= 32 bytes; longer is rejected).
 * @param sec_type  Security: 0 = open, 1 = WPA2-PSK, 2 = WPA3-SAE.
 * @param pass      NUL-terminated passphrase (may be NULL/"" for open).
 * @return ALP_OK once the connect is submitted; ALP_ERR_INVAL on an over-long
 *         SSID/passphrase; ALP_ERR_IO if the bridge stayed down across the submit
 *         retries; otherwise the mapped error.
 */
alp_status_t
cc3501e_wifi_connect_async(cc3501e_t *ctx, const char *ssid, uint8_t sec_type, const char *pass);

/**
 * @brief Read the non-blocking connection-status latch (WIFI_STATUS, opcode 0x1B).
 *
 * A SINGLE-frame, non-blocking snapshot of the STA connection state, the result
 * channel for @ref cc3501e_wifi_connect_async: CONNECTING while the association
 * runs, then CONNECTED (with @c rssi_dbm) or CONN_FAILED (with @c fail_reason).
 * While the CC35 is mid-association the bridge is BUSY, so this returns
 * @ref ALP_ERR_BUSY (the READY gate) rather than blocking -- the caller retries
 * once @ref cc3501e_bus_ready reports the bridge is ready again.
 *
 * @param ctx  Initialised driver context.
 * @param out  Receives the @ref alp_cc3501e_wifi_status_t snapshot on success.
 * @return ALP_OK with @p out filled; ALP_ERR_BUSY if the bridge is mid-radio-op
 *         (READY low); ALP_ERR_IO on a short reply; otherwise the mapped error.
 */
alp_status_t cc3501e_wifi_status(cc3501e_t *ctx, alp_cc3501e_wifi_status_t *out);

/**
 * @brief Bridge READY/host-IRQ flow-control hook -- is the CC3501E ready to clock?
 *
 * Weak default returns true (the portability fallback for a board with no READY
 * line).  A board that wires the READY/host-IRQ line (CC35 GPIO17 -> an Alif input,
 * a rev-1 wire) provides a
 * strong override that reads it: true = the SPI slave is armed and the host may
 * clock a transaction; false = the CC35 is mid-radio-op (its SPI-slave DMA is
 * dead) and the host MUST NOT clock.  @ref cc3501e_request consults it and returns
 * ALP_ERR_BUSY when it is false; a host background poller can read it directly to
 * pace an async-connect result collection (poll @ref cc3501e_wifi_status only when
 * this is true).
 *
 * @return true if the slave is armed and the host may clock a transaction;
 *         false if the CC35 is mid-radio-op and the host must not clock.
 */
bool cc3501e_bus_ready(void);

/**
 * @brief Set the request-HEADER -> request-PAYLOAD settle delay (µs).
 *
 * The LOAD-BEARING settle: the slave arms the request-PAYLOAD transfer in
 * its ISR only AFTER the header transfer completes, so payload requests (CONNECT /
 * AP / OTA_WRITE / GPIO_WRITE) DESYNC if this drops below ~200µs.  Default 200µs.
 * Affects all subsequent requests on every context.
 *
 * @param us  New settle delay in microseconds (process-global).
 */
void cc3501e_set_req_payload_settle_us(uint32_t us);

/** @brief Get the request-HEADER -> request-PAYLOAD settle delay (µs).
 *  @return The current settle delay in microseconds. */
uint32_t cc3501e_get_req_payload_settle_us(void);

/**
 * @brief Set the pre-reply-HEADER settle delay (µs).
 *
 * The throughput-sensitive settle: header-only commands (GET_VERSION / scan / ble /
 * the worker-routed ops -- the common case) have no payload phase and are reliable
 * here at near-zero, so a bench sweep can drive this toward 0 without affecting
 * payload requests.  Default 50µs.  Affects all subsequent requests on every context.
 *
 * @param us  New settle delay in microseconds (process-global).
 */
void cc3501e_set_reply_settle_us(uint32_t us);

/** @brief Get the pre-reply-HEADER settle delay (µs).
 *  @return The current settle delay in microseconds. */
uint32_t cc3501e_get_reply_settle_us(void);

/**
 * @brief Back-compat single-knob get/set over the two split settle levers.
 *
 * SET writes BOTH @ref cc3501e_set_req_payload_settle_us and @ref
 * cc3501e_set_reply_settle_us (one knob for callers that don't care about the split);
 * GET returns the throughput-sensitive reply settle (@ref cc3501e_get_reply_settle_us).
 * Prefer the split setters when tuning the reply settle independently.
 *
 * @param us  New settle delay (microseconds) applied to BOTH levers.
 * @return (getter) the current reply settle in microseconds.
 */
void cc3501e_set_phase_settle_us(uint32_t us);
/** @brief Get the back-compat single-knob settle delay (the reply settle, in µs). */
uint32_t cc3501e_get_phase_settle_us(void);

/**
 * @brief Read the current STA RSSI in dBm (WIFI_GET_RSSI, opcode 0x16).
 *
 * @param ctx   Initialised driver context.
 * @param rssi  Receives the signed dBm RSSI on success.
 * @return ALP_OK with @p rssi filled; ALP_ERR_NOT_READY if not associated
 *         (firmware RESP_ERR_NOT_READY); ALP_ERR_IO on a short reply; or the
 *         mapped error.
 *
 * @note WIRE: GET_RSSI has an opcode but NO reply payload struct in the
 *       protocol header; this helper assumes the reply data is a single
 *       int8 dBm value after the status byte.  See cc3501e.c gap note.
 */
alp_status_t cc3501e_wifi_rssi(cc3501e_t *ctx, int8_t *rssi);

/**
 * @brief Read the current STA IPv4 address (WIFI_GET_IP, opcode 0x17).
 *
 * @param ctx  Initialised driver context.
 * @param ip   Receives the 4 IPv4 octets, network order (ip[0] = MSB).
 * @return ALP_OK with @p ip filled; ALP_ERR_NOT_READY if no lease yet
 *         (firmware RESP_ERR_NOT_READY); ALP_ERR_IO on a short reply; or the
 *         mapped error.
 *
 * @note WIRE: GET_IP has an opcode but NO reply payload struct in the
 *       protocol header; this helper assumes the reply data is 4 IPv4
 *       bytes after the status byte.  See cc3501e.c gap note.
 */
alp_status_t cc3501e_wifi_get_ip(cc3501e_t *ctx, uint8_t ip[4]);

/**
 * @brief Enable the CC3501E BLE controller + NimBLE host (BLE_ENABLE, 0x30).
 *
 * The firmware worker-routes BLE_ENABLE off the SPI ISR: it brings the Wi-Fi
 * stack up first (shared HIF), then runs nimble_host_start (~2 s).  Like
 * cc3501e_wifi_get_mac, the host re-issues until the radio op completes; the
 * bridge is briefly down during the op, so @p timeout_ms is floored internally
 * to cover the bring-up window.  No reply payload -- success is the OK status.
 *
 * @param ctx         Initialised bridge handle.
 * @param timeout_ms  Caller budget (floored to the radio-down window).
 * @return ALP_OK once the BLE host is up; ALP_ERR_NOT_READY if BLE is not built
 *         in the firmware; otherwise the mapped error.
 */
alp_status_t cc3501e_ble_enable(cc3501e_t *ctx, uint32_t timeout_ms);

/**
 * @brief One parsed BLE scan (advertising-report) record handed to the caller.
 *
 * Mirrors the on-wire BLE scan record (addr[6] | addr_type | rssi(int8) |
 * name_len | name[name_len]) with the inline device name copied out into a
 * NUL-terminated buffer.  Names longer than @ref CC3501E_BLE_NAME_MAX are
 * truncated.  A device that advertises no name leaves @c name empty.
 */
#define CC3501E_BLE_NAME_MAX 31u
typedef struct {
	uint8_t addr[6];   /**< Advertiser address (LE order on the wire). */
	uint8_t addr_type; /**< NimBLE own/peer addr type (0=public,1=random,...). */
	int8_t  rssi_dbm;  /**< Advertising-report RSSI, dBm. */
	uint8_t name_len;  /**< Name length as reported on the wire. */
	char    name[CC3501E_BLE_NAME_MAX + 1u]; /**< NUL-terminated device-name copy ("" if none). */
} cc3501e_ble_scan_record_t;

/**
 * @brief Run a BLE scan and collect discovered advertisers (BLE_SCAN_START, 0x34).
 *
 * Requires the BLE controller + NimBLE host to be up (call @ref cc3501e_ble_enable
 * first).  Worker-routed in the firmware: a NimBLE @c ble_gap_disc runs for a
 * fixed window (a few seconds), de-duplicating by advertiser address, then the
 * collected reports are returned as the SCAN_START reply payload (a sequence of
 * BLE scan records, no envelope).  Poll-by-repeat absorbs the bridge-down window
 * while the radio scans, identical to @ref cc3501e_wifi_scan.
 *
 * @param ctx         Initialised bridge handle.
 * @param out_records Caller array of @p cap @ref cc3501e_ble_scan_record_t.
 * @param cap         Capacity of @p out_records.
 * @param count       Receives the number of records parsed (may be NULL).
 * @param timeout_ms  Upper bound on the poll-by-repeat budget (floored to the
 *                    firmware scan window so a slow scan is not misread as IO).
 * @return ALP_OK once the scan completed (even with zero records);
 *         ALP_ERR_NOT_READY if BLE is not enabled / not built; mapped error otherwise.
 */
alp_status_t cc3501e_ble_scan(cc3501e_t                 *ctx,
                              cc3501e_ble_scan_record_t *out_records,
                              size_t                     cap,
                              size_t                    *count,
                              uint32_t                   timeout_ms);

/* ------------------------------------------------------------------ */
/* GPIO proxy (0x50..0x53) + camera enables (0x60/0x61).              */
/*                                                                    */
/* The CC3501E fronts a set of E1M pads (IO11/IO13/IO15..IO21) and    */
/* the two camera-enable LDOs; these helpers let the host drive/read  */
/* them over the inter-chip bridge.  @p pad is the RAW CC3501E GPIO    */
/* index (the firmware drives the pad 1:1, refusing the bridge's own  */
/* SPI/UART pads); the logical IO11.. -> raw-index map lives in board  */
/* metadata, not on the wire.  These ops are synchronous + fast in the */
/* firmware (no worker), so they use the caller's @p timeout_ms with no */
/* radio-down-window floor -- but they still retry on a transient IO   */
/* (the bridge is briefly down if a radio op is running concurrently). */
/* ------------------------------------------------------------------ */

/**
 * @brief Configure a proxied CC3501E GPIO's direction + internal pull
 *        (GPIO_CONFIGURE, 0x50).
 *
 * @param ctx         Initialised bridge handle.
 * @param pad         Raw CC3501E GPIO index (e.g. 13 for GPIO13).
 * @param dir         One of @ref alp_cc3501e_gpio_direction_t.  NOTE: the CC35xx
 *                    GPIO controller has no true open-drain, so DIR_OPEN_DRAIN is
 *                    emulated as a push-pull output idling high (fine on a single-
 *                    driver line; not on a shared line).
 * @param pull        One of @ref alp_cc3501e_gpio_pull_t.
 * @param timeout_ms  Caller budget (per-request retry on transient IO).
 * @return ALP_OK on success; ALP_ERR_INVAL if @p pad is reserved/out of
 *         range; ALP_ERR_NOT_READY on a firmware build without the proxy.
 */
alp_status_t cc3501e_gpio_configure(cc3501e_t                   *ctx,
                                    uint8_t                      pad,
                                    alp_cc3501e_gpio_direction_t dir,
                                    alp_cc3501e_gpio_pull_t      pull,
                                    uint32_t                     timeout_ms);

/**
 * @brief Drive a proxied CC3501E GPIO (GPIO_WRITE, 0x51).
 *
 * @param ctx         Initialised bridge handle.
 * @param pad         Raw CC3501E GPIO index.
 * @param level       false = low, true = high (open-drain: low asserts).
 * @param timeout_ms  Caller budget.
 * @return ALP_OK on success; otherwise the mapped error.
 */
alp_status_t cc3501e_gpio_write(cc3501e_t *ctx, uint8_t pad, bool level, uint32_t timeout_ms);

/**
 * @brief Sample a proxied CC3501E GPIO (GPIO_READ, 0x52).
 *
 * @param ctx         Initialised bridge handle.
 * @param pad         Raw CC3501E GPIO index.
 * @param level_out   Receives the sampled level (false/true).
 * @param timeout_ms  Caller budget.
 * @return ALP_OK on success; ALP_ERR_INVAL if @p level_out is NULL or the
 *         pad is reserved; otherwise the mapped error.
 */
alp_status_t cc3501e_gpio_read(cc3501e_t *ctx, uint8_t pad, bool *level_out, uint32_t timeout_ms);

/**
 * @brief Arm/disable an edge interrupt on a proxied CC3501E GPIO
 *        (GPIO_SET_INTERRUPT, 0x53).
 *
 * The firmware arms the pad's HW edge interrupt; on this rev the async
 * EVT_GPIO_INTERRUPT delivery has no slave->master attention
 * line, so the edge is latched on the CC3501E for a future poll/EVT path --
 * arming the controller is real, the host notification is deferred to the
 * next board rev.
 *
 * @param ctx         Initialised bridge handle.
 * @param pad         Raw CC3501E GPIO index.
 * @param edge        One of @ref alp_cc3501e_gpio_edge_t (NONE disables).  NOTE: the
 *                    CC35xx controller has no both-edges trigger, so EDGE_BOTH
 *                    returns ALP_ERR_INVAL -- arm RISING or FALLING.
 * @param enabled     false = disable, true = enable.
 * @param timeout_ms  Caller budget.
 * @return ALP_OK once armed/disabled; otherwise the mapped error.
 */
alp_status_t cc3501e_gpio_set_interrupt(
    cc3501e_t *ctx, uint8_t pad, alp_cc3501e_gpio_edge_t edge, bool enabled, uint32_t timeout_ms);

/**
 * @brief Drive a CC3501E camera-enable LDO (CAM_ENABLE 0x60 / CAM_DISABLE 0x61).
 *
 * @param ctx         Initialised bridge handle.
 * @param which       0 = CAM_EN_LDO0 (CC35 GPIO_1), 1 = CAM_EN_LDO1 (CC35 GPIO_0)
 *                    — per the E1M-AEN BDE-BW35N U4 netlist (pins 54/55).
 * @param on          true asserts the enable, false deasserts.
 * @param timeout_ms  Caller budget.
 * @return ALP_OK on success; ALP_ERR_INVAL on a bad @p which; otherwise mapped.
 */
alp_status_t cc3501e_cam_enable(cc3501e_t *ctx, uint8_t which, bool on, uint32_t timeout_ms);

/**
 * @brief Apply a power-management policy hint to the CC3501E (POWER_POLICY, 0x62).
 *
 * Hints how aggressively the CC3501E firmware idles between events.  The coarse
 * preset maps to the CC35xx Power manager: PERFORMANCE keeps the device awake
 * (WFI only), BALANCED lets it opportunistically sleep between events, and
 * LOW_POWER / DEEP_SLEEP let it reach its deepest sleep state.  Takes effect on
 * the device's next idle-detection cycle.
 *
 * @param ctx         Initialised bridge handle.
 * @param policy      Power-policy payload: @c policy is one of
 *                    @ref alp_cc3501e_pp_preset_t; @c wake_events is a bitmap of
 *                    @c ALP_CC3501E_WAKE_* (all-zero is rejected with a low-power
 *                    preset -- keep at least @c ALP_CC3501E_WAKE_HOST_SPI);
 *                    @c idle_ms_before_sleep is the minimum idle before sleep
 *                    (0 = firmware default).  @c reserved is sent as zero.
 * @param timeout_ms  Caller budget.
 * @return ALP_OK on success; ALP_ERR_INVAL if @p policy is NULL, the preset is
 *         unknown, or a low-power preset carries no wake source; otherwise the
 *         mapped error.
 */
alp_status_t
cc3501e_power_policy(cc3501e_t *ctx, const alp_cc3501e_power_policy_t *policy, uint32_t timeout_ms);

/* ------------------------------------------------------------------ */
/* OTA firmware update -- stream a new CC3501E image over the bridge.  */
/*                                                                    */
/* The Alif host obtains a signed GPE-format vendor image (via the     */
/* device-side Mender contract; the OTA server is a separate repo) and */
/* streams it into the CC3501E's non-primary vendor slot, which the    */
/* CC35 then installs + swaps on reboot (PSA-FWU).  See                */
/* docs/cc3501e-bridge.md "OTA".                                       */
/* ------------------------------------------------------------------ */

/**
 * @brief Push a complete signed CC3501E vendor image over the bridge + install.
 *
 * Runs the full cycle: OTA_BEGIN(len) -> chunked OTA_WRITE -> OTA_FINISH.  On
 * success the CC3501E has staged the image into its non-primary vendor slot and
 * reboots so BL2 swaps it to primary (TRIAL), after which it self-accepts.  THE
 * BRIDGE LINK DROPS during that reboot: expect the link to go quiet, then
 * re-establish (cc3501e_reset / the soak) and confirm the new GET_VERSION.
 *
 * Recovers from a missed per-chunk reply by re-syncing to the device's actual
 * write cursor (CMD_OTA_STATUS) rather than blindly re-sending (OTA_WRITE is
 * not idempotent -- a re-sent already-written offset is rejected).
 *
 * @param ctx         Initialised bridge handle.
 * @param image       Signed GPE-format vendor image (manifest + body).
 * @param len         Image length in bytes (must exceed the manifest).
 * @param timeout_ms  Per-frame budget for each BEGIN / WRITE / FINISH request.
 * @return ALP_OK once FINISH is acked (the device reboots afterwards);
 *         otherwise the first failing step's status (caller may
 *         cc3501e_ota_abort() to reset the device session).
 */
alp_status_t
cc3501e_ota_update(cc3501e_t *ctx, const uint8_t *image, size_t len, uint32_t timeout_ms);

/* Granular OTA controls (cc3501e_ota_update wraps these for the common path). */

/** @brief Open an OTA session: declare @p total_len; the device picks its
 *  non-primary vendor slot and brings it READY.
 *  @param ctx        Initialised bridge handle.
 *  @param total_len  Total image length in bytes the session will stream.
 *  @param timeout_ms Per-frame budget.
 *  @return ALP_OK once the session is open; otherwise the mapped error. */
alp_status_t cc3501e_ota_begin(cc3501e_t *ctx, uint32_t total_len, uint32_t timeout_ms);

/** @brief Stream one sequential image chunk at absolute @p offset.
 *  @param ctx        Initialised bridge handle.
 *  @param offset     Absolute image offset; must equal the device write cursor.
 *  @param data       Chunk bytes.
 *  @param len        Chunk length; must be 1..ALP_CC3501E_OTA_MAX_CHUNK.
 *  @param timeout_ms Per-frame budget.
 *  @return ALP_OK once the chunk is acked; ALP_ERR_INVAL on a bad len/offset;
 *          otherwise the mapped error.  NOT idempotent (a re-sent offset is
 *          rejected) -- re-sync via @ref cc3501e_ota_status after a missed ack. */
alp_status_t cc3501e_ota_write(
    cc3501e_t *ctx, uint32_t offset, const uint8_t *data, size_t len, uint32_t timeout_ms);

/** @brief Finalize: the device installs the staged image + arms the swap reboot.
 *  @param ctx        Initialised bridge handle.
 *  @param timeout_ms Per-frame budget.
 *  @return ALP_OK once FINISH is acked (the device reboots afterwards);
 *          otherwise the mapped error. */
alp_status_t cc3501e_ota_finish(cc3501e_t *ctx, uint32_t timeout_ms);

/** @brief Cancel an in-flight session on the device.
 *  @param ctx        Initialised bridge handle.
 *  @param timeout_ms Per-frame budget.
 *  @return ALP_OK once the session is cancelled; otherwise the mapped error. */
alp_status_t cc3501e_ota_abort(cc3501e_t *ctx, uint32_t timeout_ms);

/** @brief Query device session state into @p out (state / bytes_written / total_len).
 *  @param ctx        Initialised bridge handle.
 *  @param out        Receives the device-side session snapshot.
 *  @param timeout_ms Per-frame budget.
 *  @return ALP_OK with @p out filled; otherwise the mapped error. */
alp_status_t cc3501e_ota_status(cc3501e_t *ctx, alp_cc3501e_ota_status_t *out, uint32_t timeout_ms);

/* ------------------------------------------------------------------ */
/* Portable GPIO proxy wiring (CONFIG_ALP_SDK_GPIO_CC3501E_PROXY).     */
/*                                                                    */
/* When the proxy backend is built, alp_gpio_open(pin_id) on the AEN   */
/* target routes pin_ids listed in cc3501e_gpio_routes[] through the   */
/* bridge (cc3501e_gpio_*); every other pin_id delegates to the        */
/* platform GPIO driver.  The board provides the route table (filled   */
/* from the SoM pad map); the SDK ships a WEAK empty default so an      */
/* un-mapped build delegates every pin (no behaviour change).  The app */
/* calls alp_gpio_cc3501e_attach() once after cc3501e_init() so the     */
/* proxy has a bridge handle; until then proxied pins delegate too.    */
/* ------------------------------------------------------------------ */

/** One portable-pin -> raw CC3501E GPIO index route. */
typedef struct {
	uint32_t pin_id;    /**< portable alp_gpio_open() id (board-defined). */
	uint8_t  cc35_gpio; /**< raw CC3501E GPIO index the bridge drives. */
} cc3501e_gpio_route_t;

/** Board-provided route table (WEAK empty default in the proxy backend).
 *  Populate from the SoM pad map to enable proxied IOs. */
extern const cc3501e_gpio_route_t cc3501e_gpio_routes[];
extern const size_t               cc3501e_gpio_route_count;

/**
 * @brief Attach the live bridge handle to the GPIO proxy backend.
 *
 * Call once after cc3501e_init().  Without it (or with an empty route table)
 * every alp_gpio_open() pin delegates to the platform GPIO driver, so the
 * proxy is a no-op until both a bridge is attached and the route table is
 * populated.  Only defined when CONFIG_ALP_SDK_GPIO_CC3501E_PROXY is set.
 *
 * @param ctx  Initialised bridge handle.
 * @return ALP_OK; ALP_ERR_INVAL on a NULL @p ctx.
 */
alp_status_t alp_gpio_cc3501e_attach(cc3501e_t *ctx);

/** Issue a synchronous command + wait for the response.
 *
 *  @param ctx         CC3501E driver context (must be initialised first).
 *  @param cmd         Command opcode (one of @c ALP_CC3501E_CMD_* ).
 *  @param tx_payload  Outbound payload bytes (may be NULL with len 0).
 *  @param tx_len      Outbound payload length in bytes.
 *  @param rx_buf      Reply buffer (response payload, less the
 *                     frame header).  Truncated to @p rx_cap.
 *  @param rx_cap      Capacity of @p rx_buf in bytes.
 *  @param rx_len      Receives bytes copied (may be NULL).
 *  @param timeout_ms  Max wait.
 *  @return ALP_OK on a successful round-trip; ALP_ERR_BUSY if the bridge is
 *          mid-radio-op (READY low); ALP_ERR_TIMEOUT on no reply; otherwise
 *          the mapped firmware-status error. */
alp_status_t cc3501e_request(cc3501e_t        *ctx,
                             alp_cc3501e_cmd_t cmd,
                             const uint8_t    *tx_payload,
                             size_t            tx_len,
                             uint8_t          *rx_buf,
                             size_t            rx_cap,
                             size_t           *rx_len,
                             uint32_t          timeout_ms);

/**
 * @brief Register or replace the async-event callback.
 *
 * @param ctx   Initialised driver context.
 * @param cb    Callback invoked on the RX thread per event; NULL to detach.
 * @param user  Opaque pointer passed back to @p cb.
 * @return ALP_OK on success; ALP_ERR_INVAL on a NULL @p ctx.
 */
alp_status_t cc3501e_set_event_callback(cc3501e_t *ctx, cc3501e_event_cb_t cb, void *user);

/**
 * @brief Free internal state.  Idempotent.
 *
 * Does not close the SPI bus -- the caller owns it.
 *
 * @param ctx  Driver context (may already be deinitialised, or NULL).
 */
void cc3501e_deinit(cc3501e_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CC3501E_H */
