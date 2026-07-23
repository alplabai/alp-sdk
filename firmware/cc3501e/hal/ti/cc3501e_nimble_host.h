/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * cc3501e-bridge HAL: TI backend -- NimBLE host adapter seam (v0.3 BLE).
 *
 * Thin C interface between the cc3501e_hw_ble_* HAL bodies (cc3501e_hw_ti.c)
 * and the NimBLE host glue (cc3501e_nimble_host.c).  Keeps the raw NimBLE
 * headers out of cc3501e_hw_ti.c.  Compiled only on the CC3501E_BLE build.
 */

#ifndef CC3501E_NIMBLE_HOST_H
#define CC3501E_NIMBLE_HOST_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Enable the NWP BLE controller and bring up the NimBLE host.
 *
 * Calls BleIf_EnableBLE() (shared HIF -- Wi-Fi must already be started),
 * inits the NimBLE port + LL transport + GAP/GATT/DIS services + a minimal
 * GATT server, spawns the host task, and blocks (~1 s budget) until the
 * host<->controller sync callback fires.
 *
 * @return 0 on success (host enabled); negative on failure / sync timeout.
 */
int cc3501e_nimble_host_start(void);

/**
 * @brief Query whether the NimBLE host is up and synced.
 * @return non-zero if enabled, 0 otherwise.
 */
int cc3501e_nimble_host_is_enabled(void);

/**
 * @brief Configure + start the single extended-advertising set.
 *
 * @param connectable      non-zero for a connectable adv set.
 * @param interval_min_ms  min advertising interval in ms (0 -> 100 ms default).
 * @param interval_max_ms  max advertising interval in ms (0 -> 100 ms default).
 * @param adv_data         raw AD bytes to advertise; if NULL/len 0 a default
 *                         (flags + complete device name) is built instead.
 * @param adv_data_len     length of @p adv_data (0 -> use the default).
 * @return 0 on success; negative / NimBLE error code on failure.
 */
int cc3501e_nimble_adv_config_and_start(uint8_t        connectable,
                                        uint16_t       interval_min_ms,
                                        uint16_t       interval_max_ms,
                                        const uint8_t *adv_data,
                                        uint8_t        adv_data_len);

/**
 * @brief Stop the extended-advertising set.
 * @return 0 on success; negative on failure.
 */
int cc3501e_nimble_adv_stop(void);

/**
 * @brief Tear down BLE activity (advertising + discovery).
 *
 * Best-effort: if the host is enabled, stops the ext-adv set and cancels any
 * in-flight GAP discovery (both idempotent).  A no-op if the host never came
 * up.  The NimBLE host itself stays initialised (a re-enable is a no-op).
 *
 * @return 0 always (teardown is best-effort).
 */
int cc3501e_nimble_host_disable(void);

/**
 * @brief One discovered advertiser, filled by @ref cc3501e_nimble_scan.
 *
 * HAL-internal mirror of the on-wire BLE scan record; cc3501e_hw_ble_scan()
 * packs an array of these onto the bridge reply.  @c name holds the parsed
 * (complete or short) device name, NUL-terminated ("" if the advertiser sent
 * none).
 */
typedef struct {
	uint8_t addr[6];   /**< Advertiser address (NimBLE LE order). */
	uint8_t addr_type; /**< NimBLE peer address type. */
	int8_t  rssi;      /**< Latest advertising-report RSSI, dBm. */
	uint8_t name_len;  /**< Bytes used in @c name (0..31). */
	char    name[32];  /**< NUL-terminated device name (<= 31 chars). */
} cc3501e_nimble_scan_rec_t;

/**
 * @brief Run a GAP discovery (BLE scan) and collect advertisers.
 *
 * Active scan for @p duration_ms, de-duplicated by advertiser address (latest
 * RSSI + first non-empty name kept).  Blocks until the scan window completes
 * (must be called from the worker drain, never the SPI ISR).  Requires the
 * NimBLE host to be up (@ref cc3501e_nimble_host_start).
 *
 * @param out          Caller array of @p cap records.
 * @param cap          Capacity of @p out.
 * @param out_count    Receives the number of advertisers written (may be NULL).
 * @param duration_ms  Scan window in milliseconds.
 * @return 0 on success; negative on failure (host not enabled / disc error).
 */
int cc3501e_nimble_scan(cc3501e_nimble_scan_rec_t *out,
                        uint32_t                   cap,
                        uint32_t                  *out_count,
                        uint32_t                   duration_ms);

/**
 * @brief Central-connect to a peer and block (bounded) for the GAP connection.
 *
 * Issues ble_gap_connect() and waits (~10 s budget) for BLE_GAP_EVENT_CONNECT;
 * on success the connection handle is latched for the GATT-client ops below.
 * Must be called from the worker drain (blocks), never the SPI ISR.
 *
 * @param addr_type  NimBLE peer address type (BLE_ADDR_*).
 * @param addr       6-byte peer address (LE order).
 * @return 0 on success; negative / NimBLE error code on failure or timeout.
 */
int cc3501e_nimble_connect(uint8_t addr_type, const uint8_t addr[6]);

/**
 * @brief Terminate the active connection and block for the disconnect.
 * @return 0 on success (or already disconnected); negative on failure.
 */
int cc3501e_nimble_disconnect(void);

/**
 * @brief Cancel any in-flight GAP discovery (idempotent).
 * @return 0 on success / no scan active; negative NimBLE error otherwise.
 */
int cc3501e_nimble_scan_stop(void);

/**
 * @brief GATT-client read of a peer attribute; blocks for the read response.
 * @param handle   Peer attribute handle to read.
 * @param out      Caller buffer for the attribute value.
 * @param cap      Capacity of @p out.
 * @param out_len  Receives the number of bytes written (may be NULL).
 * @return 0 on success; negative on failure (not connected / GATT error).
 */
int cc3501e_nimble_gatt_read(uint16_t handle, uint8_t *out, uint16_t cap, uint16_t *out_len);

/**
 * @brief GATT-client acknowledged write to a peer attribute; blocks for the ack.
 * @param handle  Peer attribute handle to write.
 * @param data    Payload bytes.
 * @param len     Length of @p data.
 * @return 0 on success; negative on failure (not connected / GATT error).
 */
int cc3501e_nimble_gatt_write(uint16_t handle, const uint8_t *data, uint16_t len);

/**
 * @brief Register a dynamic GATT service from a parsed BLE_GATT_REGISTER
 *        descriptor (see ALP_CC3501E_CMD_BLE_GATT_REGISTER in
 *        <alp/protocol/cc3501e.h> for the exact byte layout; @p desc /
 *        @p desc_len are already length/shape-validated by protocol_ble.c).
 *
 * Builds a struct ble_gatt_svc_def / ble_gatt_chr_def table from static
 * storage (NimBLE keeps referencing it for the service's lifetime, so it
 * cannot be a stack local), then ble_gatts_count_cfg + ble_gatts_add_svcs +
 * ble_gatts_start.  This ADDS to the fixed demo service registered at BLE
 * enable (cc3501e_gatt_svr_init) -- it does not replace it.  v1 supports
 * exactly ONE dynamic service; a second call is rejected.  See
 * cc3501e_nimble_host.c for the full NimBLE-lifecycle rationale (the
 * ble_gatts_start() re-run this needs is the vendor SDK's own documented way
 * to add services after the first one, but is unverified on real silicon as
 * of this rev -- author-only pass, no bench access).
 *
 * @param desc            Validated descriptor bytes (see the wire format).
 * @param desc_len        Length of @p desc.
 * @param handles_out     Receives one attribute VALUE handle per
 *                        characteristic, in descriptor order.
 * @param handles_cap     Capacity of @p handles_out.
 * @param num_handles_out Receives the number of handles written (0 on error).
 * @return 0 on success; negative on failure (bad descriptor, NimBLE error, a
 *         service already registered, or the host not yet enabled).
 */
int cc3501e_nimble_gatt_register(const uint8_t *desc,
                                 uint16_t       desc_len,
                                 uint16_t      *handles_out,
                                 uint16_t       handles_cap,
                                 uint16_t      *num_handles_out);

/**
 * @brief Send a GATT notification to the connected peer.
 * @param handle  Attribute value handle (0 -> the demo characteristic handle).
 * @param data    Notification payload.
 * @param len     Length of @p data.
 * @return 0 on success; negative on failure (nobody connected / mbuf alloc).
 */
int cc3501e_nimble_gatt_notify(uint16_t handle, const uint8_t *data, uint16_t len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CC3501E_NIMBLE_HOST_H */
