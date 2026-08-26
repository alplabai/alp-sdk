/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file wifi.h
 * @brief CC3501E Wi-Fi host helpers + portable Wi-Fi backend attach.
 *
 * Thin wrappers over @ref cc3501e_request that match the opcodes +
 * payloads in `<alp/protocol/cc3501e.h>`.  Several of these poll the
 * firmware "by repeat": the firmware accepts the request, kicks off a
 * worker (scan / DHCP / association), and returns ALP_ERR_BUSY
 * (RESP_ERR_BUSY) on each repeat until the worker finishes -- so the
 * host re-issues the SAME command on a bounded retry/backoff until it
 * gets RESP_OK with the result, or the timeout elapses.  This is how
 * the bring-up proves the firmware's worker seam from the host with
 * no async-event line on this HW rev.
 */

#ifndef ALP_CHIPS_CC3501E_WIFI_H
#define ALP_CHIPS_CC3501E_WIFI_H

#include <stdint.h>
#include <stddef.h>

#include "alp/chips/cc3501e/core.h"
#include "alp/protocol/cc3501e.h"

#ifdef __cplusplus
extern "C" {
#endif

/** MAC address length in bytes (the GET_MAC reply data). */
#define CC3501E_MAC_LEN 6u

/**
 * @brief Read the CC3501E's Wi-Fi station MAC address (GET_MAC, opcode 0x03).
 *
 * GET_MAC is poll-by-repeat on the firmware side: the firmware may answer
 * RESP_ERR_BUSY while the radio identity is still being read out of the
 * device, so this loops the GET_MAC request on a bounded backoff
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
	CC3501E_WIFI_SEC_OPEN    = 0,
	CC3501E_WIFI_SEC_WEP     = 1,
	CC3501E_WIFI_SEC_WPA     = 2,
	CC3501E_WIFI_SEC_WPA2    = 3,
	CC3501E_WIFI_SEC_WPA3    = 4,
	CC3501E_WIFI_SEC_UNKNOWN = 255,
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
 *         @ref ALP_ERR_NOT_READY if @p ctx is NULL or not initialised;
 *         @ref ALP_ERR_BUSY if a scan is already decoding on this SAME
 *         @p ctx (issue #740) -- a plain, non-atomic same-call-stack
 *         reentrancy guard (see @ref cc3501e_poll_events's concurrency
 *         warning; a caller polling one @p ctx from multiple threads must
 *         serialize its own calls). A different @p ctx has entirely
 *         separate storage and may always scan concurrently;
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
 * @brief Abort an in-progress Wi-Fi scan (WIFI_SCAN_STOP, opcode 0x11).
 *
 * No payload, no reply data -- success is the OK status.  The firmware tears
 * the scan down as a radio op, so the bridge can be briefly down; the budget is
 * floored internally to the radio down-window and the request is re-issued on a
 * bounded backoff until it lands, like @ref cc3501e_wifi_get_mac.
 *
 * @param ctx  Initialised driver context.
 * @return ALP_OK once the firmware acknowledged the stop; ALP_ERR_TIMEOUT if it
 *         stayed busy for the whole down-window; mapped error otherwise.
 */
alp_status_t cc3501e_wifi_scan_stop(cc3501e_t *ctx);

/**
 * @brief Named security selectors for @ref cc3501e_wifi_connect /
 *        @ref cc3501e_wifi_ap_start's @c sec_type parameter (matches
 *        @ref alp_cc3501e_wifi_connect_t::security on the wire).
 *
 * Typed @c uint8_t (not a bare enum) so a caller-side conditional picking
 * between them (e.g. "open if no PSK, else WPA2-PSK") assigns cleanly to a
 * @c uint8_t with no implicit-int narrowing conversion -- see issue #742.
 */
#define CC3501E_WIFI_CONNECT_SEC_OPEN     ((uint8_t)0u)
#define CC3501E_WIFI_CONNECT_SEC_WPA2_PSK ((uint8_t)1u)
#define CC3501E_WIFI_CONNECT_SEC_WPA3_SAE ((uint8_t)2u)

/**
 * @brief Associate with a Wi-Fi AP (WIFI_CONNECT_STA, opcode 0x12).
 *
 * Submits the SSID / security / passphrase ONCE, as the on-wire
 * @ref alp_cc3501e_wifi_connect_t header followed by the inline SSID then the
 * inline passphrase -- the firmware is fire-and-forget for this opcode (it
 * acks the submit with RESP_ERR_BUSY and never replies synchronously again on
 * it), so the submit is not retried.  The outcome is then awaited off the
 * independent, non-blocking @ref cc3501e_wifi_status latch (CONNECTING while
 * the association runs, CONNECTED / CONN_FAILED once it resolves) rather than
 * trusted off the submit's own acknowledgement -- see issues #1376 / #1378:
 * the old poll-by-repeat-the-submit contract could re-submit a fresh
 * association on every retry and could alias a dead bus phase as a false
 * "connected".
 *
 * If this call FOLLOWS a failed attempt (the @ref cc3501e_wifi_status latch
 * still reads CONN_FAILED when this is entered) it first clears that stale
 * association -- @ref cc3501e_wifi_disconnect() -- before submitting, else
 * the new association's own Wlan_Connect kick fails against the leftover NWP
 * state (#1435).  That clear is bounded by @c CC3501E_WIFI_DOWN_WINDOW_MS
 * IN ADDITION to @p timeout_ms, the same floor-regardless-of-caller-budget
 * precedent as @ref cc3501e_wifi_get_mac's down-window floor. A live
 * CONNECTING attempt or an already-CONNECTED association is left alone.
 *
 * @param ctx         Initialised driver context.
 * @param ssid        NUL-terminated SSID (<= 32 bytes; longer is rejected).
 * @param sec_type    Security selector -- @ref CC3501E_WIFI_CONNECT_SEC_OPEN /
 *                    _WPA2_PSK / _WPA3_SAE (matches
 *                    @ref alp_cc3501e_wifi_connect_t::security on the wire).
 * @param pass        NUL-terminated passphrase (may be NULL/"" for open).
 * @param timeout_ms  Upper bound on the WIFI_STATUS poll budget (the submit
 *                    itself uses a short, fixed, non-retried timeout). A call
 *                    that clears a stale association first (see above) also
 *                    spends up to @c CC3501E_WIFI_DOWN_WINDOW_MS on that
 *                    clear, on top of this budget.
 * @return ALP_OK once WIFI_STATUS reports CONNECTED; ALP_ERR_TIMEOUT if still
 *         CONNECTING (or unreadable) at the deadline, or if the firmware
 *         reports CONN_FAILED with fail_reason == FAIL_TIMEOUT;
 *         ALP_ERR_INVAL on an over-long SSID/passphrase or a synchronous
 *         submit reject; ALP_ERR_IO if the firmware reports CONN_FAILED for
 *         any other reason.
 */
alp_status_t cc3501e_wifi_connect(cc3501e_t  *ctx,
                                  const char *ssid,
                                  uint8_t     sec_type,
                                  const char *pass,
                                  uint32_t    timeout_ms);

/**
 * @brief Tear down the STA association (WIFI_DISCONNECT, opcode 0x13).
 *
 * No payload, no reply data -- success is the OK status.  Disconnect is a radio
 * op (Wlan_Disconnect), so the bridge can be briefly down while it runs; the
 * budget is floored internally to the radio down-window and the request is
 * re-issued on a bounded backoff until it lands, like @ref cc3501e_wifi_get_mac.
 *
 * @param ctx  Initialised driver context.
 * @return ALP_OK once the firmware acknowledged the disconnect; ALP_ERR_TIMEOUT
 *         if it stayed busy for the whole down-window; mapped error otherwise.
 */
alp_status_t cc3501e_wifi_disconnect(cc3501e_t *ctx);

/**
 * @brief Start a Wi-Fi soft-AP (WIFI_AP_START, opcode 0x14).
 *
 * Brings the CC3501E up as an access point advertising @p ssid.  The on-wire
 * payload is identical to @ref cc3501e_wifi_connect -- an
 * @ref alp_cc3501e_wifi_connect_t header (ssid_len / psk_len / security)
 * followed by the inline SSID then the inline passphrase, packed with no
 * padding.  Unlike @ref cc3501e_wifi_connect this issues the submit exactly
 * ONCE and returns immediately (issue #1696): AP_START is fire-and-forget on
 * the firmware side (every submit is acked RESP_ERR_BUSY, and the job slot
 * that would carry a WORKER_DONE outcome is reset to IDLE before the host may
 * clock again), and unlike CONNECT_STA there is no independent status latch
 * to poll afterwards -- the firmware's AP path never writes the WIFI_STATUS
 * connection latch.  A retry loop around this opcode is therefore PROVABLY
 * unwinnable: every attempt reads back either the BUSY ack (no progress) or
 * the dead-phase all-zero alias, which is rejected as ALP_ERR_IO at the
 * transport (no progress either) -- there is no reply this opcode can ever
 * produce that decodes as ALP_OK, so polling only spends wall-clock time
 * (and re-issues, see the warning below) without changing the answer.
 *
 * @warning Against CC3501E firmware protocol v4 this call CANNOT report
 *          success, and returns ALP_ERR_TIMEOUT for an AP that came up
 *          perfectly.  Treat ALP_ERR_TIMEOUT as fully inconclusive, NOT as
 *          "submitted, unconfirmed": it covers the expected BUSY submit ack,
 *          the rejected dead-phase alias, AND at least three cases where
 *          nothing ever reached the wire -- a RESP_ERR_BUSY bounce because a
 *          different worker op (scan / get_mac / BLE) was already in flight,
 *          a transport IO fault during the radio-down window, and a
 *          request-lock timeout under a concurrent caller -- all
 *          indistinguishable from here.  A caller that treats ALP_ERR_TIMEOUT
 *          as proof of submission will not retry when it should; confirm the
 *          AP out of band and be prepared to retry until #1696 lands a
 *          firmware-side confirmation channel.  @ref ALP_ERR_INVAL and
 *          @ref ALP_ERR_NOT_READY, by contrast, ARE conclusive: both are
 *          local or synchronous rejects that mean nothing was submitted.
 *
 * @param ctx         Initialised driver context.
 * @param ssid        NUL-terminated AP SSID (<= 32 bytes; longer is rejected).
 * @param sec_type    Security selector -- @ref CC3501E_WIFI_CONNECT_SEC_OPEN /
 *                    _WPA2_PSK / _WPA3_SAE (matches
 *                    @ref alp_cc3501e_wifi_connect_t::security on the wire).
 * @param pass        NUL-terminated passphrase (may be NULL/"" for an open AP).
 * @param timeout_ms  Currently unused (the submit is not retried -- there is
 *                    no independent channel to bound a wait on); reserved for
 *                    a future firmware-side confirmation channel, kept in the
 *                    signature so that addition needs no API break.
 * @return ALP_ERR_TIMEOUT for the expected outcome -- see the warning above:
 *         conclusive for neither success nor failure, and NOT proof the
 *         submit ever reached the wire; ALP_ERR_INVAL on an over-long
 *         SSID/passphrase or a synchronous submit reject; ALP_ERR_NOT_READY
 *         if @p ctx is NULL or not initialised.  ALP_OK is not reachable
 *         against protocol v4.
 */
alp_status_t cc3501e_wifi_ap_start(cc3501e_t  *ctx,
                                   const char *ssid,
                                   uint8_t     sec_type,
                                   const char *pass,
                                   uint32_t    timeout_ms);

/**
 * @brief Stop the Wi-Fi soft-AP (WIFI_AP_STOP, opcode 0x15).
 *
 * No payload, no reply data -- success is the OK status.  Tearing the AP down
 * is a radio op, so the bridge can be briefly down while it runs; the budget is
 * floored internally to the radio down-window and the request is re-issued on a
 * bounded backoff until it lands, like @ref cc3501e_wifi_disconnect.
 *
 * @warning The status depends on whether there was actually an AP to stop, and
 *          only the no-op case is conclusive.  Bench-measured on E1M-AEN801
 *          (2026-08-18), reproducibly:
 *
 *          - **No soft-AP running** -> ALP_OK.  The firmware DOES acknowledge
 *            this opcode, so ALP_OK is reachable and means what it says.
 *          - **A soft-AP actually running** -> ALP_ERR_TIMEOUT, every time.
 *            Tearing the AP down takes the bridge through the radio-down
 *            window and the acknowledgement is not observed across it, so the
 *            call cannot report on the outcome it was asked about (#1553; the
 *            same shape @ref cc3501e_wifi_ap_start documents under #1696).
 *
 *          So ALP_ERR_TIMEOUT here is INCONCLUSIVE, and it is precisely the
 *          case a caller cares about: not proof the teardown failed, not proof
 *          it succeeded.  Confirm out of band if it matters.  Note also that
 *          this sequence is a reliable way to wedge the transport -- an
 *          `ap start` followed by `ap-stop` left the bridge returning
 *          ALP_ERR_IO to every subsequent request until it was reset.
 *
 * @param ctx  Initialised driver context.
 * @return ALP_OK when there was no soft-AP to stop (the firmware acknowledges
 *         that case, so this is conclusive); ALP_ERR_TIMEOUT when an AP WAS
 *         running -- see the warning above: not proof the teardown failed, and
 *         not proof it succeeded; ALP_ERR_NOT_READY if @p ctx is NULL or not
 *         initialised; mapped error otherwise.
 */
alp_status_t cc3501e_wifi_ap_stop(cc3501e_t *ctx);

/**
 * @brief Read the current STA RSSI in dBm (WIFI_GET_RSSI, opcode 0x16).
 *
 * Worker-routed on the firmware side (a fresh request lazy-starts the radio,
 * which can take seconds): re-issues on a bounded backoff, floored to the
 * radio down-window, while the firmware reports RESP_ERR_BUSY -- see issue
 * #1377, where a single non-retried request here collected only the
 * submit's own BUSY ack and left the job orphaned.
 *
 * @warning As with @ref cc3501e_wifi_status, the retry rides out a
 *          transient down-window only -- it cannot resync or reset a
 *          genuinely wedged transport, and will burn the whole down-window
 *          before reporting a timeout in that case rather than failing fast.
 *
 * @param ctx   Initialised driver context.
 * @param rssi  Receives the signed dBm RSSI on success.
 * @return ALP_OK with @p rssi filled; ALP_ERR_NOT_READY if not associated
 *         (firmware RESP_ERR_NOT_READY); ALP_ERR_TIMEOUT if it stayed busy
 *         for the whole down-window; ALP_ERR_IO on a short reply; or the
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
 * @brief Poll the non-blocking STA connection state (WIFI_STATUS, opcode 0x1B).
 *
 * Reads the firmware's connection-state latch without a radio op (ISR-safe on
 * the firmware side): how the host collects the outcome of an async
 * @ref cc3501e_wifi_connect submit -- CONNECTING while the association runs,
 * then CONNECTED or FAILED once the WLAN connect event lands.  The reply is the
 * fixed 4-byte @ref alp_cc3501e_wifi_status_t wire layout (state | fail_reason |
 * rssi_dbm | reserved), decoded into @p out.
 *
 * The firmware-side latch read is itself non-blocking, but the SHARED bridge
 * transport is briefly down whenever any radio op is in flight (a connect
 * this same call may be trying to observe the outcome of, included) -- see
 * issue #1377, where a status read taken moments after a connect desynced
 * like any other transaction that lands in that window.  This is therefore
 * poll-by-repeat internally, floored to the radio down-window, even though
 * the WIFI_STATUS opcode itself never replies RESP_ERR_BUSY.
 *
 * @warning The retry only rides out a TRANSIENT down-window (the bridge
 *          briefly unavailable while a radio op runs) -- it re-issues the
 *          identical SPI transaction with no resync, no CS recovery, no
 *          reset, so it cannot recover a genuinely WEDGED transport.  If
 *          @ref cc3501e_get_version (a plain, non-worker, short-timeout
 *          request) also fails, the transport itself is broken rather than
 *          mid-radio-op, and this call will still burn the whole down-window
 *          (bounded, but up to several seconds) before reporting
 *          @c ALP_ERR_TIMEOUT / @c ALP_ERR_IO rather than failing fast.
 *          Recovering a wedged link needs a different remedy (link resync or
 *          reset) that this driver does not yet implement.
 *
 * @param ctx  Initialised driver context.
 * @param out  Receives the decoded status snapshot: @c state is a
 *             @ref alp_cc3501e_wifi_conn_state_t and @c fail_reason a
 *             @ref alp_cc3501e_wifi_fail_t (valid when state == CONN_FAILED).
 *             @c rssi_dbm is decoded faithfully off the wire but is NOT a
 *             measurement -- the firmware latch behind it is never populated
 *             and the byte is always 0, which is itself a legal RSSI, so a
 *             caller cannot tell it apart from a real reading (issue #1387).
 *             Use @ref cc3501e_wifi_rssi for a signal level, and report it as
 *             unavailable when that call fails rather than printing a 0.
 * @return ALP_OK with @p out filled; ALP_ERR_INVAL if @p out is NULL;
 *         ALP_ERR_TIMEOUT if the transport stayed down for the whole
 *         down-window; ALP_ERR_IO on a short reply; otherwise the mapped
 *         error.
 */
alp_status_t cc3501e_wifi_status(cc3501e_t *ctx, alp_cc3501e_wifi_status_t *out);

/**
 * @brief Attach the live bridge handle to the portable Wi-Fi backend.
 *
 * Only defined when CONFIG_ALP_SDK_WIFI_CC3501E is set.  After a successful
 * attach, @ref alp_wifi_open routes through the CC3501E backend on AEN silicon.
 *
 * @param ctx  Initialised bridge handle.
 * @return ALP_OK; ALP_ERR_INVAL on a NULL or uninitialised @p ctx.
 */
alp_status_t alp_wifi_cc3501e_attach(cc3501e_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CC3501E_WIFI_H */
