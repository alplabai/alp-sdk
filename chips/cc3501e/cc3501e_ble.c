/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * CC3501E BLE host helpers (opcodes 0x30..0x3B).  See
 * <alp/chips/cc3501e/ble.h> for the public API.
 *
 * Thin wrappers over cc3501e_request matching the firmware BLE
 * handlers (firmware/cc3501e/src/protocol.c handle_ble_*).  WIRE GAP:
 * the protocol header carries the BLE opcodes + alp_cc3501e_ble_adv_
 * start_t, but has NO payload struct for CONNECT (0x36) or the four
 * GATT ops (0x38..0x3B); those layouts are defined only by the
 * firmware handlers and are transcribed per-helper below (the same
 * precedent as the Wi-Fi GET_MAC / scan wire-gap notes in
 * cc3501e_wifi.c).  These ops are direct (non-worker-routed) in the
 * firmware, so they take the caller's timeout; poll_by_repeat still
 * absorbs a transient ALP_ERR_IO if a radio op happens to overlap
 * (bridge briefly down).
 */

#include <string.h>
#include <stdint.h>

#include "cc3501e_internal.h"

/* BLE enable stands the bridge down for the NWP BLE-controller cold-init (a HIF
 * control-cmd round-trip, ~10-15 s) + the NimBLE host sync; floor the host poll well
 * above that so a slow-but-working enable is not misread as a timeout. */
#define CC3501E_BLE_ENABLE_WINDOW_MS 90000u

alp_status_t cc3501e_ble_enable(cc3501e_t *ctx, uint32_t timeout_ms)
{
	/* Worker-routed: the firmware SUSPENDS the bridge SPI, runs BleIf_EnableBLE (the
	 * NWP BLE-controller cold-init -- a control-cmd round-trip that can take ~10-15 s)
	 * + nimble_host_start sync, then RE-OPENS the bridge.  The link is DOWN for that
	 * whole window, so the host must poll-by-repeat (retry on IO) longer than the
	 * 10 s Wi-Fi floor -- floor to 30 s so a working-but-slow enable is not misread as
	 * a failure before the worker publishes the result + the bridge re-syncs. */
	uint32_t budget = timeout_ms;
	if (budget < CC3501E_BLE_ENABLE_WINDOW_MS) budget = CC3501E_BLE_ENABLE_WINDOW_MS;
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_BLE_ENABLE, NULL, 0, NULL, 0, NULL, budget);
}

/* Wire BLE scan record: addr[6] | addr_type | rssi(int8) | name_len | name[name_len]
 * (see cc3501e_ble_scan_record_t). Fixed 9-byte header, name packed inline. */
#define CC3501E_BLE_REC_HDR 9u

alp_status_t cc3501e_ble_scan(cc3501e_t                 *ctx,
                              cc3501e_ble_scan_record_t *out_records,
                              size_t                     cap,
                              size_t                    *count,
                              uint32_t                   timeout_ms)
{
	if (out_records == NULL && cap > 0u) return ALP_ERR_INVAL;
	if (count != NULL) *count = 0;

	/* Mirror of cc3501e_wifi_scan: the firmware returns the advertising
	 * reports it collected as the BLE_SCAN_START reply payload. */
	static uint8_t scan_buf[ALP_CC3501E_MAX_PAYLOAD];
	size_t         got = 0;
	alp_status_t   s   = poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_BLE_SCAN_START, NULL, 0, scan_buf, sizeof(scan_buf), &got, timeout_ms);
	if (s != ALP_OK) return s;

	size_t off = 0;
	size_t n   = 0;
	while (off + CC3501E_BLE_REC_HDR <= got && n < cap) {
		const uint8_t *rec      = &scan_buf[off];
		uint8_t        name_len = rec[8];
		if (off + CC3501E_BLE_REC_HDR + (size_t)name_len > got) {
			break; /* truncated trailing record -- stop cleanly */
		}
		cc3501e_ble_scan_record_t *out = &out_records[n];
		memcpy(out->addr, &rec[0], 6);
		out->addr_type = rec[6];
		out->rssi_dbm  = (int8_t)rec[7];
		out->name_len  = name_len;
		uint8_t copy = (name_len > CC3501E_BLE_NAME_MAX) ? (uint8_t)CC3501E_BLE_NAME_MAX : name_len;
		memcpy(out->name, &rec[CC3501E_BLE_REC_HDR], copy);
		out->name[copy] = '\0';
		off += CC3501E_BLE_REC_HDR + (size_t)name_len;
		n++;
	}
	if (count != NULL) *count = n;
	return ALP_OK;
}

alp_status_t cc3501e_ble_disable(cc3501e_t *ctx, uint32_t timeout_ms)
{
	/* BLE_DISABLE (0x31): no payload (firmware handle_ble_disable rejects any
	 * non-empty body).  No reply data -- success is the OK status. */
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_BLE_DISABLE, NULL, 0, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ble_adv_start(cc3501e_t     *ctx,
                                   bool           connectable,
                                   uint16_t       interval_min_ms,
                                   uint16_t       interval_max_ms,
                                   const uint8_t *adv_data,
                                   uint8_t        adv_data_len,
                                   uint32_t       timeout_ms)
{
	if (adv_data == NULL && adv_data_len > 0u) return ALP_ERR_INVAL;

	/* Hand-pack the 7-byte wire header (firmware handle_ble_adv_start
	 * BLE_ADV_START_HDR), NOT the doc struct alp_cc3501e_ble_adv_start_t which is
	 * 8 bytes with a u16-alignment pad the wire omits:
	 *   connectable(1) | reserved(1,=0) | interval_min_ms(LE16) |
	 *   interval_max_ms(LE16) | adv_data_len(1) | adv_data[adv_data_len]. */
	uint8_t buf[7u + 255u];
	buf[0] = connectable ? 1u : 0u;
	buf[1] = 0u;
	buf[2] = (uint8_t)(interval_min_ms & 0xFFu);
	buf[3] = (uint8_t)((interval_min_ms >> 8) & 0xFFu);
	buf[4] = (uint8_t)(interval_max_ms & 0xFFu);
	buf[5] = (uint8_t)((interval_max_ms >> 8) & 0xFFu);
	buf[6] = adv_data_len;
	if (adv_data_len > 0u) {
		memcpy(&buf[7], adv_data, adv_data_len);
	}
	return poll_by_repeat(ctx,
	                      ALP_CC3501E_CMD_BLE_ADV_START,
	                      buf,
	                      (size_t)7u + adv_data_len,
	                      NULL,
	                      0,
	                      NULL,
	                      timeout_ms);
}

alp_status_t cc3501e_ble_adv_stop(cc3501e_t *ctx, uint32_t timeout_ms)
{
	/* BLE_ADV_STOP (0x33): no payload. */
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_BLE_ADV_STOP, NULL, 0, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ble_scan_stop(cc3501e_t *ctx, uint32_t timeout_ms)
{
	/* BLE_SCAN_STOP (0x35): no payload. */
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_BLE_SCAN_STOP, NULL, 0, NULL, 0, NULL, timeout_ms);
}

alp_status_t
cc3501e_ble_connect(cc3501e_t *ctx, const uint8_t addr[6], uint8_t addr_type, uint32_t timeout_ms)
{
	if (addr == NULL) return ALP_ERR_INVAL;
	/* BLE_CONNECT (0x36) wire (firmware handle_ble_connect, req_len == 7):
	 * addr_type(1) | addr[6].  NOTE the addr_type-FIRST order.  No header struct
	 * -- the layout is defined only by the firmware handler. */
	uint8_t req[7];
	req[0] = addr_type;
	memcpy(&req[1], addr, 6);
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_BLE_CONNECT, req, sizeof(req), NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ble_disconnect(cc3501e_t *ctx, uint32_t timeout_ms)
{
	/* BLE_DISCONNECT (0x37): no payload (firmware handle_ble_disconnect rejects
	 * any body -- the firmware tracks the single active connection itself). */
	return poll_by_repeat(ctx, ALP_CC3501E_CMD_BLE_DISCONNECT, NULL, 0, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ble_gatt_register(cc3501e_t     *ctx,
                                       const uint8_t *descriptor,
                                       size_t         len,
                                       uint32_t       timeout_ms)
{
	if (descriptor == NULL || len == 0u || len > ALP_CC3501E_MAX_PAYLOAD) return ALP_ERR_INVAL;
	/* BLE_GATT_REGISTER (0x38): the firmware handle_ble_gatt_register takes the
	 * payload as an OPAQUE attribute-table descriptor (>= 1 byte) -- there is no
	 * header struct and no fixed UUID/handle layout on the host side; the host
	 * ships the descriptor bytes verbatim and the firmware parses them. */
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_BLE_GATT_REGISTER, descriptor, len, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ble_gatt_notify(cc3501e_t     *ctx,
                                     uint16_t       handle,
                                     const uint8_t *data,
                                     size_t         len,
                                     uint32_t       timeout_ms)
{
	if (data == NULL && len > 0u) return ALP_ERR_INVAL;
	if (len > (size_t)(ALP_CC3501E_MAX_PAYLOAD - 2u)) return ALP_ERR_INVAL;
	/* BLE_GATT_NOTIFY (0x39) wire (firmware handle_ble_gatt_notify): handle(LE16)
	 * | value bytes.  No header struct -- layout from the firmware handler. */
	uint8_t buf[2u + (ALP_CC3501E_MAX_PAYLOAD - 2u)];
	buf[0] = (uint8_t)(handle & 0xFFu);
	buf[1] = (uint8_t)((handle >> 8) & 0xFFu);
	if (len > 0u) memcpy(&buf[2], data, len);
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_BLE_GATT_NOTIFY, buf, 2u + len, NULL, 0, NULL, timeout_ms);
}

alp_status_t cc3501e_ble_gatt_read(cc3501e_t *ctx,
                                   uint16_t   handle,
                                   uint8_t   *out,
                                   size_t     cap,
                                   size_t    *out_len,
                                   uint32_t   timeout_ms)
{
	if (out == NULL && cap > 0u) return ALP_ERR_INVAL;
	if (out_len != NULL) *out_len = 0;
	/* BLE_GATT_READ (0x3A) wire (firmware handle_ble_gatt_read, req_len == 2):
	 * request = handle(LE16); the reply DATA (after the status byte) is the
	 * attribute value bytes.  No header struct -- layout from the firmware
	 * handler.  poll_by_repeat copies the value into @out (capped at @cap). */
	uint8_t req[2];
	req[0] = (uint8_t)(handle & 0xFFu);
	req[1] = (uint8_t)((handle >> 8) & 0xFFu);
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_BLE_GATT_READ, req, sizeof(req), out, cap, out_len, timeout_ms);
}

alp_status_t cc3501e_ble_gatt_write(cc3501e_t     *ctx,
                                    uint16_t       handle,
                                    const uint8_t *data,
                                    size_t         len,
                                    uint32_t       timeout_ms)
{
	if (data == NULL && len > 0u) return ALP_ERR_INVAL;
	if (len > (size_t)(ALP_CC3501E_MAX_PAYLOAD - 2u)) return ALP_ERR_INVAL;
	/* BLE_GATT_WRITE (0x3B) wire (firmware handle_ble_gatt_write): handle(LE16) |
	 * value bytes -- identical framing to NOTIFY. */
	uint8_t buf[2u + (ALP_CC3501E_MAX_PAYLOAD - 2u)];
	buf[0] = (uint8_t)(handle & 0xFFu);
	buf[1] = (uint8_t)((handle >> 8) & 0xFFu);
	if (len > 0u) memcpy(&buf[2], data, len);
	return poll_by_repeat(
	    ctx, ALP_CC3501E_CMD_BLE_GATT_WRITE, buf, 2u + len, NULL, 0, NULL, timeout_ms);
}
