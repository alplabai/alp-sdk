/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ble.h
 * @brief CC3501E BLE host helpers (enable/scan + control, opcodes 0x30..0x3B)
 *        and the portable BLE backend attach.
 *
 * Thin wrappers over the firmware BLE handlers.  WIRE GAP: the protocol
 * header carries the BLE opcodes + alp_cc3501e_ble_adv_start_t, but has NO
 * payload struct for CONNECT (0x36) or the four GATT ops (0x38..0x3B); those
 * wire layouts are defined only by the firmware handlers
 * (firmware/cc3501e/src/protocol.c handle_ble_*) and are documented
 * per-function + in cc3501e.c.  GATT async notifications
 * (EVT_BLE_GATT_WRITE_REQ, 0x3F) need the async-event path (not wired on
 * this HW rev); these wrappers issue the outbound commands only.
 */

#ifndef ALP_CHIPS_CC3501E_BLE_H
#define ALP_CHIPS_CC3501E_BLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "alp/chips/cc3501e/core.h"
#include "alp/protocol/cc3501e.h"

#ifdef __cplusplus
extern "C" {
#endif

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
 *         @ref ALP_ERR_BUSY if a scan is already decoding on this SAME
 *         @p ctx (issue #740) -- a plain, non-atomic same-call-stack
 *         reentrancy guard (see @ref cc3501e_poll_events's concurrency
 *         warning; a caller polling one @p ctx from multiple threads must
 *         serialize its own calls). A different @p ctx has entirely
 *         separate storage and may always scan concurrently;
 *         ALP_ERR_NOT_READY if BLE is not enabled / not built; mapped error otherwise.
 */
alp_status_t cc3501e_ble_scan(cc3501e_t                 *ctx,
                              cc3501e_ble_scan_record_t *out_records,
                              size_t                     cap,
                              size_t                    *count,
                              uint32_t                   timeout_ms);

/**
 * @brief Disable the CC3501E BLE controller + NimBLE host (BLE_DISABLE, 0x31).
 *
 * Undoes @ref cc3501e_ble_enable.  No payload; success is the OK status.
 *
 * @param ctx         Initialised bridge handle.
 * @param timeout_ms  Caller budget (per-request retry on transient IO).
 * @return ALP_OK once BLE is torn down; ALP_ERR_NOT_READY if BLE was not
 *         enabled / not built into the firmware; otherwise the mapped error.
 */
alp_status_t cc3501e_ble_disable(cc3501e_t *ctx, uint32_t timeout_ms);

/**
 * @brief Start BLE advertising (BLE_ADV_START, 0x32).
 *
 * Packs the 7-byte wire header the firmware @c handle_ble_adv_start parses
 * (connectable | reserved | interval_min_ms(LE16) | interval_max_ms(LE16) |
 * adv_data_len) followed by the inline advertising-data bytes.  NOTE: the doc
 * struct @ref alp_cc3501e_ble_adv_start_t is 8 bytes with an alignment pad the
 * wire omits, so the header is hand-packed to 7 bytes (see cc3501e.c).
 *
 * @param ctx              Initialised bridge handle (BLE must be enabled).
 * @param connectable      true = connectable advertising, false = non-connectable.
 * @param interval_min_ms  Minimum advertising interval, milliseconds.
 * @param interval_max_ms  Maximum advertising interval, milliseconds.
 * @param adv_data         Advertising-data bytes (may be NULL only if
 *                         @p adv_data_len is 0).
 * @param adv_data_len     Advertising-data length (0..255).
 * @param timeout_ms       Caller budget.
 * @return ALP_OK once advertising is up; ALP_ERR_INVAL on a NULL @p adv_data
 *         with a non-zero length; ALP_ERR_NOT_READY if BLE is not enabled;
 *         otherwise the mapped error.
 */
alp_status_t cc3501e_ble_adv_start(cc3501e_t     *ctx,
                                   bool           connectable,
                                   uint16_t       interval_min_ms,
                                   uint16_t       interval_max_ms,
                                   const uint8_t *adv_data,
                                   uint8_t        adv_data_len,
                                   uint32_t       timeout_ms);

/**
 * @brief Stop BLE advertising (BLE_ADV_STOP, 0x33).
 *
 * @param ctx         Initialised bridge handle.
 * @param timeout_ms  Caller budget.
 * @return ALP_OK once advertising is stopped; otherwise the mapped error.
 */
alp_status_t cc3501e_ble_adv_stop(cc3501e_t *ctx, uint32_t timeout_ms);

/**
 * @brief Stop an in-progress BLE scan (BLE_SCAN_STOP, 0x35).
 *
 * Cancels the NimBLE GAP discovery started by @ref cc3501e_ble_scan before its
 * window elapses.  No payload; success is the OK status.
 *
 * @param ctx         Initialised bridge handle.
 * @param timeout_ms  Caller budget.
 * @return ALP_OK once the scan is stopped; otherwise the mapped error.
 */
alp_status_t cc3501e_ble_scan_stop(cc3501e_t *ctx, uint32_t timeout_ms);

/**
 * @brief Initiate a BLE central connection to a peer (BLE_CONNECT, 0x36).
 *
 * The wire payload the firmware @c handle_ble_connect parses is
 * addr_type(1) | addr[6] (7 bytes; addr_type FIRST).  No header struct exists
 * for this opcode -- the layout is defined only by the firmware handler.
 *
 * @param ctx         Initialised bridge handle (BLE must be enabled).
 * @param addr        6-byte peer address (little-endian, as carried on the wire
 *                    and reported by @ref cc3501e_ble_scan_record_t::addr).
 * @param addr_type   Peer address type (0 = public, 1 = random, per NimBLE).
 * @param timeout_ms  Caller budget.
 * @return ALP_OK once the connection is established; ALP_ERR_INVAL on a NULL
 *         @p addr; ALP_ERR_NOT_READY if BLE is not enabled; otherwise mapped.
 */
alp_status_t
cc3501e_ble_connect(cc3501e_t *ctx, const uint8_t addr[6], uint8_t addr_type, uint32_t timeout_ms);

/**
 * @brief Disconnect the active BLE connection (BLE_DISCONNECT, 0x37).
 *
 * No payload -- the firmware tracks the single active connection itself.
 *
 * @param ctx         Initialised bridge handle.
 * @param timeout_ms  Caller budget.
 * @return ALP_OK once disconnected; otherwise the mapped error.
 */
alp_status_t cc3501e_ble_disconnect(cc3501e_t *ctx, uint32_t timeout_ms);

/**
 * @brief Register a GATT attribute table with the firmware (BLE_GATT_REGISTER, 0x38).
 *
 * The firmware @c handle_ble_gatt_register takes the payload as an OPAQUE
 * attribute-table descriptor (>= 1 byte): there is no host-side header struct
 * and no fixed UUID/handle layout on the wire, so the host ships the descriptor
 * bytes verbatim and the firmware parses them.
 *
 * @param ctx         Initialised bridge handle (BLE must be enabled).
 * @param descriptor  Opaque attribute-table descriptor bytes (non-NULL).
 * @param len         Descriptor length (1..@ref ALP_CC3501E_MAX_PAYLOAD).
 * @param timeout_ms  Caller budget.
 * @return ALP_OK once registered; ALP_ERR_INVAL on a NULL/empty/oversized
 *         descriptor; otherwise the mapped error.
 */
alp_status_t cc3501e_ble_gatt_register(cc3501e_t     *ctx,
                                       const uint8_t *descriptor,
                                       size_t         len,
                                       uint32_t       timeout_ms);

/**
 * @brief Send a GATT notification on a characteristic (BLE_GATT_NOTIFY, 0x39).
 *
 * Wire (firmware @c handle_ble_gatt_notify): handle(LE16) | value bytes.  No
 * header struct -- the layout is defined only by the firmware handler.
 *
 * @param ctx         Initialised bridge handle (BLE must be enabled + connected).
 * @param handle      Characteristic-value attribute handle to notify on.
 * @param data        Value bytes to notify (may be NULL only if @p len is 0).
 * @param len         Value length (0..@ref ALP_CC3501E_MAX_PAYLOAD minus 2).
 * @param timeout_ms  Caller budget.
 * @return ALP_OK once the notification is queued; ALP_ERR_INVAL on a bad arg /
 *         oversized value; otherwise the mapped error.
 */
alp_status_t cc3501e_ble_gatt_notify(cc3501e_t     *ctx,
                                     uint16_t       handle,
                                     const uint8_t *data,
                                     size_t         len,
                                     uint32_t       timeout_ms);

/**
 * @brief Read a GATT attribute value (BLE_GATT_READ, 0x3A).
 *
 * Wire request (firmware @c handle_ble_gatt_read): handle(LE16); the reply DATA
 * (after the status byte) is the attribute value bytes.  No header struct --
 * the layout is defined only by the firmware handler.
 *
 * @param ctx         Initialised bridge handle (BLE must be enabled + connected).
 * @param handle      Attribute handle to read.
 * @param out         Receives the value bytes (may be NULL only if @p cap is 0);
 *                    truncated to @p cap.
 * @param cap         Capacity of @p out in bytes.
 * @param out_len     Receives the number of value bytes copied (may be NULL).
 * @param timeout_ms  Caller budget.
 * @return ALP_OK with @p out / @p out_len filled; ALP_ERR_INVAL on a NULL
 *         @p out with non-zero @p cap; otherwise the mapped error.
 */
alp_status_t cc3501e_ble_gatt_read(cc3501e_t *ctx,
                                   uint16_t   handle,
                                   uint8_t   *out,
                                   size_t     cap,
                                   size_t    *out_len,
                                   uint32_t   timeout_ms);

/**
 * @brief Write a GATT attribute value (BLE_GATT_WRITE, 0x3B).
 *
 * Wire (firmware @c handle_ble_gatt_write): handle(LE16) | value bytes --
 * identical framing to @ref cc3501e_ble_gatt_notify.  No header struct.
 *
 * @param ctx         Initialised bridge handle (BLE must be enabled + connected).
 * @param handle      Attribute handle to write.
 * @param data        Value bytes to write (may be NULL only if @p len is 0).
 * @param len         Value length (0..@ref ALP_CC3501E_MAX_PAYLOAD minus 2).
 * @param timeout_ms  Caller budget.
 * @return ALP_OK once the write is accepted; ALP_ERR_INVAL on a bad arg /
 *         oversized value; otherwise the mapped error.
 */
alp_status_t cc3501e_ble_gatt_write(cc3501e_t     *ctx,
                                    uint16_t       handle,
                                    const uint8_t *data,
                                    size_t         len,
                                    uint32_t       timeout_ms);

/**
 * @brief Attach the live bridge handle to the portable BLE backend.
 *
 * Only defined when CONFIG_ALP_SDK_BLE_CC3501E is set.  After a successful
 * attach, @ref alp_ble_open routes through the CC3501E backend on AEN silicon.
 *
 * @param ctx  Initialised bridge handle.
 * @return ALP_OK; ALP_ERR_INVAL on a NULL or uninitialised @p ctx.
 */
alp_status_t alp_ble_cc3501e_attach(cc3501e_t *ctx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_CHIPS_CC3501E_BLE_H */
