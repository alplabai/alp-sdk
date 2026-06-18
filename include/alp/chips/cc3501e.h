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
 *  in `<alp/protocol/cc3501e.h>`. */
typedef void (*cc3501e_event_cb_t)(uint8_t cmd, const uint8_t *payload, size_t len, void *user);

struct cc3501e {
	bool               initialised;
	alp_spi_t         *bus;        /**< SPI1 to the CC3501E (Alif master). */
	alp_gpio_t        *enable_pin; /**< WIFI.EN (P15_5).  May be NULL on boards that tie it on. */
	alp_gpio_t        *reset_pin;  /**< E_WIFI.NRST (P15_1_FLEX). */
	cc3501e_event_cb_t event_cb;
	void              *event_user;
	uint8_t            rx_scratch[ALP_CC3501E_HEADER_BYTES + ALP_CC3501E_MAX_PAYLOAD];
	uint8_t            tx_scratch[ALP_CC3501E_HEADER_BYTES + ALP_CC3501E_MAX_PAYLOAD];
};

/**
 * @brief Initialise the driver and bind it to an open SPI1 bus.
 *
 * Does not enable the radio -- call @ref cc3501e_reset to bring
 * the firmware up.  @p bus must remain valid for the lifetime
 * of @p ctx.
 */
alp_status_t cc3501e_init(cc3501e_t *ctx, alp_spi_t *bus);

/** Pulse the firmware's reset line then de-assert WIFI.EN.  Blocks
 *  until the firmware's PING reply arrives or the timeout expires. */
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
 * @brief (Re)establish byte alignment on the CS-less 3-wire link.
 *
 * With no chip-select to delimit transactions, the master and slave keep
 * framing by fixed clock count alone; a missed/extra clock (or a slave
 * that booted mid-transaction) leaves them byte-misaligned with no edge to
 * recover on.  This walks the SPI byte phase until it observes the slave's
 * header-idle marker (@ref ALP_CC3501E_SYNC_IDLE, driven only when the
 * slave is parked at a clean request-header boundary), confirming with two
 * consecutive aligned reads to reject a stray marker byte inside reply
 * data.  Call it before the first request after reset, and on any
 * desync the request path detects (reply header that doesn't echo the
 * command).
 *
 * @param ctx         Initialised driver context.
 * @param timeout_ms  Coarse upper bound on re-sync effort (each ~ms covers
 *                    one full-frame byte-walk attempt).
 * @return ALP_OK once aligned; ALP_ERR_TIMEOUT if the slave never parked
 *         (e.g. unpowered / not running its firmware).
 */
alp_status_t cc3501e_sync(cc3501e_t *ctx, uint32_t timeout_ms);

/** Retrieve the firmware's protocol version (compare against
 *  `ALP_CC3501E_PROTOCOL_VERSION` to confirm wire compatibility). */
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
	uint8_t bssid[6];                    /**< AP BSSID (MAC). */
	int8_t  rssi_dbm;                    /**< Received signal strength, dBm. */
	uint8_t channel;                     /**< Wi-Fi channel. */
	uint8_t security;                    /**< 0 = open, 1 = WPA2-PSK, 2 = WPA3-SAE. */
	uint8_t ssid_len;                    /**< SSID length as reported on the wire. */
	char    ssid[CC3501E_SSID_MAX + 1u]; /**< NUL-terminated SSID copy. */
} cc3501e_scan_record_t;

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
 *  @param timeout_ms  Max wait. */
alp_status_t cc3501e_request(cc3501e_t        *ctx,
                             alp_cc3501e_cmd_t cmd,
                             const uint8_t    *tx_payload,
                             size_t            tx_len,
                             uint8_t          *rx_buf,
                             size_t            rx_cap,
                             size_t           *rx_len,
                             uint32_t          timeout_ms);

/** Register or replace the async-event callback.  Pass cb=NULL to detach. */
alp_status_t cc3501e_set_event_callback(cc3501e_t *ctx, cc3501e_event_cb_t cb, void *user);

/** Free internal state.  Does not close the SPI bus -- caller owns it. */
void cc3501e_deinit(cc3501e_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CC3501E_H */
