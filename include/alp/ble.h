/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ble.h
 * @brief Alp SDK Bluetooth Low Energy abstraction (peripheral + central).
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
 *
 * Typical peripheral-role usage:
 * @code
 *     alp_ble_t *ble = alp_ble_open();
 *     alp_ble_uuid_t srv = {.b = {...}};
 *     alp_ble_advertise_start(ble, &(alp_ble_adv_config_t){
 *         .name = "MyDevice",
 *         .services = &srv,
 *         .num_services = 1,
 *         .interval_min_ms = 100,
 *         .interval_max_ms = 200,
 *         .connectable = true,
 *     });
 * @endcode
 *
 * @par ABI status: [ABI-STABLE]
 *      v0.2 decl + v0.3 impl; advertise + connect + GATT-read shape stable.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_BLE_H
#define ALP_BLE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "alp/peripheral.h"
#include "alp/cap_instance.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 128-bit UUID (little-endian byte order, BT-SIG convention). */
typedef struct {
	uint8_t b[16]; /**< Raw 128-bit value, little-endian (low byte first). */
} alp_ble_uuid_t;

/** Peer Bluetooth address with type discrimination. */
typedef struct {
	uint8_t type;    /**< 0 = public, 1 = random static, 2 = random private. */
	uint8_t addr[6]; /**< Little-endian (low byte first), per BT spec. */
} alp_ble_addr_t;

/** Opaque BLE host handle.  Singleton. */
typedef struct alp_ble alp_ble_t;

/** Opaque BLE connection handle. */
typedef struct alp_ble_conn alp_ble_conn_t;

/**
 * @brief Acquire the BLE host singleton.
 *
 * Subsequent calls return the same pointer (BLE is system-singleton on
 * every supported backend).  The first call triggers controller init;
 * subsequent calls are cheap.
 *
 * @return Non-NULL on success, or NULL if the controller failed to
 *         initialise (radio absent, RF blocked, etc.).
 */
alp_ble_t *alp_ble_open(void);

/**
 * @brief Release the host (last close shuts the controller down).  NULL safe.
 *
 * @param[in] ble  Handle from @ref alp_ble_open, or NULL.
 */
void alp_ble_close(alp_ble_t *ble);

/* ------------------------------------------------------------------ */
/* Peripheral role                                                     */
/* ------------------------------------------------------------------ */

/** Advertising configuration. */
typedef struct {
	const char           *name;            /**< Local name in adv data (≤ 29 chars). */
	const alp_ble_uuid_t *services;        /**< Array of advertised service UUIDs. */
	size_t                num_services;    /**< Length of @c services. */
	uint16_t              interval_min_ms; /**< Min advertising interval, milliseconds. */
	uint16_t              interval_max_ms; /**< Max advertising interval, milliseconds. */
	bool                  connectable;     /**< true = connectable adv; false = non-connectable. */
} alp_ble_adv_config_t;

/**
 * @brief Begin advertising with the given parameters.
 *
 * @param[in] ble  Host handle from @ref alp_ble_open.
 * @param[in] cfg  Advertising configuration.  Must be non-NULL.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_BUSY
 *         (already advertising) / ALP_ERR_IO.
 */
alp_status_t alp_ble_advertise_start(alp_ble_t *ble, const alp_ble_adv_config_t *cfg);

/**
 * @brief Stop advertising.  Idempotent.
 *
 * @param[in] ble  Host handle from @ref alp_ble_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY / ALP_ERR_IO.
 */
alp_status_t alp_ble_advertise_stop(alp_ble_t *ble);

/** GATT characteristic property bits (BT-SIG values). */
#define ALP_BLE_GATT_PROP_READ     0x02
#define ALP_BLE_GATT_PROP_WRITE    0x08
#define ALP_BLE_GATT_PROP_NOTIFY   0x10
#define ALP_BLE_GATT_PROP_INDICATE 0x20

/** GATT attribute handle (host-stack-assigned). */
typedef uint16_t alp_ble_attr_handle_t;

/** Single characteristic in a service definition. */
typedef struct {
	alp_ble_uuid_t uuid;          /**< Characteristic UUID. */
	uint8_t        properties;    /**< OR of ALP_BLE_GATT_PROP_*. */
	const uint8_t *initial_value; /**< Initial attribute value, or NULL for empty. */
	size_t         initial_len;   /**< Length of @c initial_value in bytes. */
} alp_ble_char_def_t;

/** GATT service definition: one UUID + an array of characteristics. */
typedef struct {
	alp_ble_uuid_t            service_uuid; /**< Primary service UUID. */
	const alp_ble_char_def_t *chars;        /**< Array of characteristic definitions. */
	size_t                    num_chars;    /**< Length of @c chars. */
} alp_ble_service_def_t;

/**
 * @brief Register a primary service with its characteristics.
 *
 * On success populates @p handles_out with one attribute handle per
 * characteristic in declaration order.
 *
 * @param[in]  ble          Host handle from @ref alp_ble_open.
 * @param[in]  def          Service + characteristics.  Must be non-NULL.
 * @param[out] handles_out  Receives the attribute handles for each
 *                          characteristic.  Caller-allocated array of
 *                          @c def->num_chars elements.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_NOMEM.
 */
alp_status_t alp_ble_gatt_register_service(alp_ble_t                   *ble,
                                           const alp_ble_service_def_t *def,
                                           alp_ble_attr_handle_t       *handles_out);

/**
 * @brief Push a notify (or indicate, if the char is configured for it)
 *        to the connected peer.
 *
 * No-op if the peer has not subscribed to this characteristic.
 *
 * @param[in] ble       Host handle.
 * @param[in] conn      Connection handle from @ref alp_ble_connect.
 * @param[in] handle    Characteristic attribute handle.
 * @param[in] payload   Notification payload bytes.
 * @param[in] len       Payload length, ≤ ATT_MTU − 3.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL.
 */
alp_status_t alp_ble_gatt_notify(alp_ble_t            *ble,
                                 alp_ble_conn_t       *conn,
                                 alp_ble_attr_handle_t handle,
                                 const uint8_t        *payload,
                                 size_t                len);

/* ------------------------------------------------------------------ */
/* Central role                                                        */
/* ------------------------------------------------------------------ */

/** One advertising packet observed by the scanner. */
typedef struct {
	alp_ble_addr_t addr;     /**< Advertiser address. */
	int8_t         rssi_dbm; /**< Received signal strength, dBm. */
	uint8_t        adv_type; /**< Adv PDU type (0..4). */
	const uint8_t *adv_data; /**< Raw advertising payload; valid only during the callback. */
	size_t         adv_len;  /**< Length of @c adv_data in bytes. */
} alp_ble_scan_result_t;

/** Scan-result callback.  Runs on the BLE host thread. */
typedef void (*alp_ble_scan_cb_t)(const alp_ble_scan_result_t *r, void *user);

/**
 * @brief Start scanning for advertisements.
 *
 * @param[in] ble     Host handle.
 * @param[in] active  true → send SCAN_REQ; false → passive scan only.
 * @param[in] cb      Per-packet callback.  Must not be NULL.
 * @param[in] user    Opaque pointer forwarded to @p cb.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_BUSY.
 */
alp_status_t alp_ble_scan_start(alp_ble_t *ble, bool active, alp_ble_scan_cb_t cb, void *user);

/**
 * @brief Stop scanning.  Idempotent.
 *
 * @param[in] ble  Host handle from @ref alp_ble_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY / ALP_ERR_IO.
 */
alp_status_t alp_ble_scan_stop(alp_ble_t *ble);

/**
 * @brief Initiate a connection to @p peer.
 *
 * Blocks up to @p timeout_ms for connection establishment.
 *
 * @param[in]  ble         Host handle.
 * @param[in]  peer        Address of the peripheral to connect to.
 * @param[in]  timeout_ms  Max wait.
 * @param[out] conn_out    Receives the connection handle on success.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL /
 *         ALP_ERR_TIMEOUT / ALP_ERR_IO.
 */
alp_status_t alp_ble_connect(alp_ble_t            *ble,
                             const alp_ble_addr_t *peer,
                             uint32_t              timeout_ms,
                             alp_ble_conn_t      **conn_out);

/**
 * @brief Tear down an active connection.
 *
 * @param[in] conn  Connection handle from @ref alp_ble_connect.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_IO.
 */
alp_status_t alp_ble_disconnect(alp_ble_conn_t *conn);

/**
 * @brief Synchronously read a characteristic by handle.
 *
 * @param[in]  conn        Connection handle.
 * @param[in]  handle      Attribute handle returned during service discovery.
 * @param[out] out         Destination buffer.
 * @param[in]  out_cap     Buffer capacity in bytes.
 * @param[out] out_len     Receives the byte count read.  May be NULL.
 * @param[in]  timeout_ms  Max wait.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_TIMEOUT / ALP_ERR_IO.
 */
alp_status_t alp_ble_gatt_read(alp_ble_conn_t       *conn,
                               alp_ble_attr_handle_t handle,
                               uint8_t              *out,
                               size_t                out_cap,
                               size_t               *out_len,
                               uint32_t              timeout_ms);

/**
 * @brief Synchronously write a characteristic by handle (with response).
 *
 * @param[in] conn        Connection handle.
 * @param[in] handle      Attribute handle.
 * @param[in] data        Source bytes.
 * @param[in] len         Length, ≤ ATT_MTU − 3.
 * @param[in] timeout_ms  Max wait.
 * @return ALP_OK / ALP_ERR_NOT_READY / ALP_ERR_INVAL / ALP_ERR_TIMEOUT / ALP_ERR_IO.
 */
alp_status_t alp_ble_gatt_write(alp_ble_conn_t       *conn,
                                alp_ble_attr_handle_t handle,
                                const uint8_t        *data,
                                size_t                len,
                                uint32_t              timeout_ms);

/**
 * @brief Query the capabilities of an opened BLE radio handle.
 *
 * @param ble  Handle from @ref alp_ble_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p ble is NULL.
 */
const alp_capabilities_t *alp_ble_capabilities(const alp_ble_t *ble);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_BLE_H */
