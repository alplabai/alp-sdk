/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ble.h
 * @brief ALP SDK Bluetooth Low Energy abstraction (peripheral + central).
 *
 * v0.3 deliverable.  v0.1 ships only the public surface; every entry
 * point returns ALP_ERR_NOSUPPORT and `*_open()` returns NULL.
 *
 * Backends (locked decision per the v0.1 brainstorming output):
 *   - Zephyr   : Zephyr `bt` host stack + the active SoC's BLE controller.
 *   - Yocto    : BlueZ over D-Bus.
 *   - Baremetal: ALP_ERR_NOSUPPORT (BLE off the bare-metal v0.1 path).
 *
 * The header takes a deliberately small slice of the BLE feature surface
 * — peripheral advertise + GATT server, central scan + connect + GATT
 * client.  Mesh, audio, and direction-finding are explicitly out of
 * scope for v1.0 and would arrive as separate `<alp/ble_mesh.h>` /
 * `<alp/ble_audio.h>` headers.
 */

#ifndef ALP_BLE_H
#define ALP_BLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 128-bit UUID (little-endian byte order, BT-SIG convention). */
typedef struct {
    uint8_t b[16];
} alp_ble_uuid_t;

/** Peer address.  Type 0 = public, 1 = random static, 2 = random private. */
typedef struct {
    uint8_t type;
    uint8_t addr[6];    /**< Little-endian (low byte first), per BT spec. */
} alp_ble_addr_t;

typedef struct alp_ble alp_ble_t;
typedef struct alp_ble_conn alp_ble_conn_t;

/** Acquire the BLE host singleton.  Subsequent calls return the same
 *  pointer (BLE is system-singleton on every supported backend). */
alp_ble_t   *alp_ble_open(void);
void         alp_ble_close(alp_ble_t *ble);

/* ------------------------------------------------------------------ */
/* Peripheral role                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    const char    *name;            /**< Local name in adv data (≤ 29 chars). */
    const alp_ble_uuid_t *services; /**< Array of advertised service UUIDs. */
    size_t         num_services;
    uint16_t       interval_min_ms;
    uint16_t       interval_max_ms;
    bool           connectable;
} alp_ble_adv_config_t;

alp_status_t alp_ble_advertise_start(alp_ble_t *ble,
                                     const alp_ble_adv_config_t *cfg);
alp_status_t alp_ble_advertise_stop(alp_ble_t *ble);

/** GATT characteristic property bits (BT-SIG values). */
#define ALP_BLE_GATT_PROP_READ      0x02
#define ALP_BLE_GATT_PROP_WRITE     0x08
#define ALP_BLE_GATT_PROP_NOTIFY    0x10
#define ALP_BLE_GATT_PROP_INDICATE  0x20

typedef uint16_t alp_ble_attr_handle_t;

typedef struct {
    alp_ble_uuid_t uuid;
    uint8_t        properties;      /**< OR of ALP_BLE_GATT_PROP_*. */
    const uint8_t *initial_value;
    size_t         initial_len;
} alp_ble_char_def_t;

typedef struct {
    alp_ble_uuid_t              service_uuid;
    const alp_ble_char_def_t   *chars;
    size_t                      num_chars;
} alp_ble_service_def_t;

/** Register a primary service with its characteristics.  On success
 *  populates @p handles_out with one attribute handle per characteristic
 *  in declaration order. */
alp_status_t alp_ble_gatt_register_service(alp_ble_t *ble,
                                           const alp_ble_service_def_t *def,
                                           alp_ble_attr_handle_t *handles_out);

/** Push a notify (or indicate, if the char is configured for it) to the
 *  connected peer.  No-op if the peer has not subscribed. */
alp_status_t alp_ble_gatt_notify(alp_ble_t *ble,
                                 alp_ble_conn_t *conn,
                                 alp_ble_attr_handle_t handle,
                                 const uint8_t *payload,
                                 size_t len);

/* ------------------------------------------------------------------ */
/* Central role                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    alp_ble_addr_t addr;
    int8_t         rssi_dbm;
    uint8_t        adv_type;
    const uint8_t *adv_data;
    size_t         adv_len;
} alp_ble_scan_result_t;

typedef void (*alp_ble_scan_cb_t)(const alp_ble_scan_result_t *r, void *user);

alp_status_t alp_ble_scan_start(alp_ble_t *ble,
                                bool active,
                                alp_ble_scan_cb_t cb, void *user);
alp_status_t alp_ble_scan_stop(alp_ble_t *ble);

alp_status_t alp_ble_connect(alp_ble_t *ble,
                             const alp_ble_addr_t *peer,
                             uint32_t timeout_ms,
                             alp_ble_conn_t **conn_out);
alp_status_t alp_ble_disconnect(alp_ble_conn_t *conn);

/** Synchronously read a characteristic by handle. */
alp_status_t alp_ble_gatt_read(alp_ble_conn_t *conn,
                               alp_ble_attr_handle_t handle,
                               uint8_t *out, size_t out_cap,
                               size_t *out_len,
                               uint32_t timeout_ms);

/** Synchronously write a characteristic by handle (with response). */
alp_status_t alp_ble_gatt_write(alp_ble_conn_t *conn,
                                alp_ble_attr_handle_t handle,
                                const uint8_t *data, size_t len,
                                uint32_t timeout_ms);

#ifdef __cplusplus
}  /* extern "C" */
#endif

#endif  /* ALP_BLE_H */
