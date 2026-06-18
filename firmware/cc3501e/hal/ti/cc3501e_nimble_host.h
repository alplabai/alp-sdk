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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* CC3501E_NIMBLE_HOST_H */
