/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge firmware: BLE 5.4 command-family handlers (0x30..0x3F).
 * Split out of protocol.c (issue #461); protocol_dispatch() in
 * protocol.c still owns the single command-family switch that routes
 * here.
 *
 * BLE payloads with multi-byte fields are parsed field-by-field from
 * the PACKED wire (LE), not by casting to the doc structs in
 * <alp/protocol/cc3501e.h> -- those structs carry uint16 alignment
 * padding that the wire format does not.
 */

#include "protocol_internal.h"

/* BLE_ENABLE (0x30): bring up the BLE stack.  Worker-routed (P0-4 seam): the
 * real CC3501E_BLE body starts Wi-Fi first (shared HIF) then nimble_host_start,
 * which blocks for SECONDS -- MUST NOT run in the SPI ISR that dispatches this
 * handler.  Poll-by-repeat, identical to GET_MAC (see handle_worker_routed).
 * Argless reply (the OK carries no data; min_cap 0). */
alp_cc3501e_resp_t handle_ble_enable(const uint8_t *req,
                                     size_t         req_len,
                                     uint8_t       *reply_data,
                                     size_t         reply_cap,
                                     size_t        *reply_data_len)
{
	(void)req;
	return handle_worker_routed(
	    ALP_CC3501E_CMD_BLE_ENABLE, 0u, req_len, reply_data, reply_cap, reply_data_len);
}

/* BLE_DISABLE (0x31): tear down advertising + scanning.  Worker-routed
 * (argless), identical to BLE_ENABLE/SCAN: the real body issues HCI over the
 * shared HIF and re-syncs the bridge SPI, both of which block and so MUST NOT
 * run in the SPI ISR that dispatches this handler (see handle_worker_routed). */
alp_cc3501e_resp_t handle_ble_disable(const uint8_t *req,
                                      size_t         req_len,
                                      uint8_t       *reply_data,
                                      size_t         reply_cap,
                                      size_t        *reply_data_len)
{
	(void)req;
	return handle_worker_routed(
	    ALP_CC3501E_CMD_BLE_DISABLE, 0u, req_len, reply_data, reply_cap, reply_data_len);
}

/* BLE_ADV_START (0x32): packed wire = connectable(1) | reserved(1) |
 * interval_min_ms(LE16) | interval_max_ms(LE16) | adv_data_len(1) |
 * adv_data[adv_data_len].  (7-byte header; the doc struct is 8 with pad.)
 *
 * Length-validated HERE, then WORKER-ROUTED with its payload (like
 * WIFI_CONNECT_STA): the real cc3501e_hw_ble_adv_start issues ext-adv HCI and
 * blocks on the shared-HIF ack, which MUST NOT run in the SPI ISR that
 * dispatches this handler (that is the -4/adv-wedge root cause).  The raw req
 * bytes are stashed via worker_submit_payload; worker_execute re-derives the
 * 7-byte header + adv_data in the drain (see worker.c). */
#define BLE_ADV_START_HDR 7u
alp_cc3501e_resp_t handle_ble_adv_start(const uint8_t *req,
                                        size_t         req_len,
                                        uint8_t       *reply_data,
                                        size_t         reply_cap,
                                        size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len < BLE_ADV_START_HDR) return ALP_CC3501E_RESP_ERR_INVALID;
	const uint8_t adv_data_len = req[6];
	if (req_len != (size_t)BLE_ADV_START_HDR + adv_data_len) return ALP_CC3501E_RESP_ERR_INVALID;
	return handle_worker_routed_payload(
	    ALP_CC3501E_CMD_BLE_ADV_START, req, req_len, reply_data_len);
}

/* BLE_ADV_STOP (0x33): stop the adv set.  Worker-routed (argless): the real
 * cc3501e_hw_ble_adv_stop issues HCI over the shared HIF + re-syncs the bridge
 * SPI, which block and so MUST NOT run in the SPI ISR (see handle_worker_routed). */
alp_cc3501e_resp_t handle_ble_adv_stop(const uint8_t *req,
                                       size_t         req_len,
                                       uint8_t       *reply_data,
                                       size_t         reply_cap,
                                       size_t        *reply_data_len)
{
	(void)req;
	return handle_worker_routed(
	    ALP_CC3501E_CMD_BLE_ADV_STOP, 0u, req_len, reply_data, reply_cap, reply_data_len);
}

/* BLE_SCAN_START (0x34): reply data = the packed advertiser list (each record
 * addr[6] | addr_type(1) | rssi(1) | name_len(1) then name_len name bytes --
 * the cc3501e_ble_scan host parser's wire format).  Worker-routed: the NimBLE
 * GAP discovery blocks for the scan window and MUST run off the SPI ISR (see
 * handle_worker_routed), exactly like WIFI_SCAN_START. */
alp_cc3501e_resp_t handle_ble_scan_start(const uint8_t *req,
                                         size_t         req_len,
                                         uint8_t       *reply_data,
                                         size_t         reply_cap,
                                         size_t        *reply_data_len)
{
	(void)req;
	return handle_worker_routed(
	    ALP_CC3501E_CMD_BLE_SCAN_START, 0u, req_len, reply_data, reply_cap, reply_data_len);
}

alp_cc3501e_resp_t handle_ble_scan_stop(const uint8_t *req,
                                        size_t         req_len,
                                        uint8_t       *reply_data,
                                        size_t         reply_cap,
                                        size_t        *reply_data_len)
{
	(void)req;
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_ble_scan_stop());
}

/* BLE_CONNECT (0x36): packed wire = addr_type(1) | addr[6].  Length-validated
 * HERE, then WORKER-ROUTED with its payload (like WIFI_CONNECT_STA): the real
 * body issues a GAP connect that blocks on the connection-complete HCI event
 * over the shared HIF and so MUST NOT run in the SPI ISR that dispatches this
 * handler.  worker_execute re-derives addr_type/addr from job.req. */
alp_cc3501e_resp_t handle_ble_connect(const uint8_t *req,
                                      size_t         req_len,
                                      uint8_t       *reply_data,
                                      size_t         reply_cap,
                                      size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 7u) return ALP_CC3501E_RESP_ERR_INVALID;
	return handle_worker_routed_payload(ALP_CC3501E_CMD_BLE_CONNECT, req, req_len, reply_data_len);
}

alp_cc3501e_resp_t handle_ble_disconnect(const uint8_t *req,
                                         size_t         req_len,
                                         uint8_t       *reply_data,
                                         size_t         reply_cap,
                                         size_t        *reply_data_len)
{
	(void)req;
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len != 0u) return ALP_CC3501E_RESP_ERR_INVALID;
	return hw_to_resp(cc3501e_hw_ble_disconnect());
}

/* BLE_GATT_REGISTER (0x38): opaque attribute-table descriptor (>= 1 byte).
 * Length-validated HERE, then WORKER-ROUTED with its payload: the real body
 * registers services + issues HCI over the shared HIF (blocks), so it MUST NOT
 * run in the SPI ISR.  worker_execute forwards the whole payload as the desc. */
alp_cc3501e_resp_t handle_ble_gatt_register(const uint8_t *req,
                                            size_t         req_len,
                                            uint8_t       *reply_data,
                                            size_t         reply_cap,
                                            size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len < 1u) return ALP_CC3501E_RESP_ERR_INVALID;
	return handle_worker_routed_payload(
	    ALP_CC3501E_CMD_BLE_GATT_REGISTER, req, req_len, reply_data_len);
}

/* BLE_GATT_NOTIFY (0x39) / WRITE (0x3B): packed wire = handle(LE16) | data.
 * Length-validated HERE, then WORKER-ROUTED with the payload (handle + data):
 * the real bodies push a notification / GATT write that blocks on HCI over the
 * shared HIF, so they MUST NOT run in the SPI ISR.  worker_execute re-derives
 * the handle + data span from job.req. */
alp_cc3501e_resp_t handle_ble_gatt_notify(const uint8_t *req,
                                          size_t         req_len,
                                          uint8_t       *reply_data,
                                          size_t         reply_cap,
                                          size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len < 2u) return ALP_CC3501E_RESP_ERR_INVALID;
	return handle_worker_routed_payload(
	    ALP_CC3501E_CMD_BLE_GATT_NOTIFY, req, req_len, reply_data_len);
}

alp_cc3501e_resp_t handle_ble_gatt_write(const uint8_t *req,
                                         size_t         req_len,
                                         uint8_t       *reply_data,
                                         size_t         reply_cap,
                                         size_t        *reply_data_len)
{
	(void)reply_data;
	(void)reply_cap;
	*reply_data_len = 0u;
	if (req_len < 2u) return ALP_CC3501E_RESP_ERR_INVALID;
	return handle_worker_routed_payload(
	    ALP_CC3501E_CMD_BLE_GATT_WRITE, req, req_len, reply_data_len);
}

/* BLE_GATT_READ (0x3A): packed wire = handle(LE16); reply data = attr value.
 * Length-validated HERE, then WORKER-ROUTED with payload AND reply: the real
 * body issues a GATT read that blocks on the read-response HCI over the shared
 * HIF, so it MUST NOT run in the SPI ISR.  Unlike the other GATT ops this one
 * returns data, so it uses the payload+reply worker path -- the worker copies
 * the attribute value into reply_data (see handle_worker_routed_payload_reply);
 * worker_execute re-derives the handle from job.req. */
alp_cc3501e_resp_t handle_ble_gatt_read(const uint8_t *req,
                                        size_t         req_len,
                                        uint8_t       *reply_data,
                                        size_t         reply_cap,
                                        size_t        *reply_data_len)
{
	*reply_data_len = 0u;
	if (req_len != 2u) return ALP_CC3501E_RESP_ERR_INVALID;
	return handle_worker_routed_payload_reply(
	    ALP_CC3501E_CMD_BLE_GATT_READ, req, req_len, 0u, reply_data, reply_cap, reply_data_len);
}
