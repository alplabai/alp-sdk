/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v0.1 stub for <alp/ble.h>.  Every entry point returns
 * ALP_ERR_NOSUPPORT; alp_ble_open returns NULL.  Real Zephyr-bt
 * implementation lands in v0.3 (see VERSIONS.md).
 *
 * Same shape as audio_stub.c / iot_stub.c / camera_stub.c.
 */

#include "alp/ble.h"

alp_ble_t *alp_ble_open(void) {
    return NULL;
}

void alp_ble_close(alp_ble_t *ble) {
    (void)ble;
}

/* ------------------------------------------------------------------ */
/* Peripheral role                                                     */
/* ------------------------------------------------------------------ */

alp_status_t alp_ble_advertise_start(alp_ble_t *ble,
                                     const alp_ble_adv_config_t *cfg) {
    (void)ble; (void)cfg;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_ble_advertise_stop(alp_ble_t *ble) {
    (void)ble;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_ble_gatt_register_service(alp_ble_t *ble,
                                           const alp_ble_service_def_t *def,
                                           alp_ble_attr_handle_t *handles_out) {
    (void)ble; (void)def; (void)handles_out;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_ble_gatt_notify(alp_ble_t *ble,
                                 alp_ble_conn_t *conn,
                                 alp_ble_attr_handle_t handle,
                                 const uint8_t *payload,
                                 size_t len) {
    (void)ble; (void)conn; (void)handle; (void)payload; (void)len;
    return ALP_ERR_NOSUPPORT;
}

/* ------------------------------------------------------------------ */
/* Central role                                                        */
/* ------------------------------------------------------------------ */

alp_status_t alp_ble_scan_start(alp_ble_t *ble,
                                bool active,
                                alp_ble_scan_cb_t cb, void *user) {
    (void)ble; (void)active; (void)cb; (void)user;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_ble_scan_stop(alp_ble_t *ble) {
    (void)ble;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_ble_connect(alp_ble_t *ble,
                             const alp_ble_addr_t *peer,
                             uint32_t timeout_ms,
                             alp_ble_conn_t **conn_out) {
    (void)ble; (void)peer; (void)timeout_ms;
    if (conn_out != NULL) *conn_out = NULL;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_ble_disconnect(alp_ble_conn_t *conn) {
    (void)conn;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_ble_gatt_read(alp_ble_conn_t *conn,
                               alp_ble_attr_handle_t handle,
                               uint8_t *out, size_t out_cap,
                               size_t *out_len,
                               uint32_t timeout_ms) {
    (void)conn; (void)handle; (void)out; (void)out_cap; (void)timeout_ms;
    if (out_len != NULL) *out_len = 0;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_ble_gatt_write(alp_ble_conn_t *conn,
                                alp_ble_attr_handle_t handle,
                                const uint8_t *data, size_t len,
                                uint32_t timeout_ms) {
    (void)conn; (void)handle; (void)data; (void)len; (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
}
