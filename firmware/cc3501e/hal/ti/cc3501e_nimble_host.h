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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CC3501E_NIMBLE_HOST_H */
