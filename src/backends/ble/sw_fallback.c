/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software BLE fallback.  Wildcard backend at priority 0 -- picked
 * only when no hardware backend is linked into the build
 * (native_sim trimmed-image case).  No real BLE controller exists
 * under native_sim, so the stub lets examples that include
 * <alp/ble.h> compile and exercise the dispatcher without pulling
 * in CONFIG_BT.
 *
 * Contract:
 *   - open()              -> ALP_OK (no controller bring-up)
 *   - advertise_start     -> ALP_OK (does nothing observable: no peers)
 *   - advertise_stop      -> ALP_OK
 *   - scan_start          -> ALP_OK (callback never fires)
 *   - scan_stop           -> ALP_OK
 *   - connect             -> ALP_ERR_NOT_IMPLEMENTED (no real peer)
 *   - everything else     -> ALP_ERR_NOT_IMPLEMENTED
 *
 * Matches the design spec Section 5 sw_fallback contract.
 *
 * @par Cost: ROM ~300 B, zero RAM (no per-handle state).
 * @par Performance: O(1) per call; every op short-circuits to
 *      ALP_OK / ALP_ERR_NOT_IMPLEMENTED with no Zephyr-subsystem
 *      touch.
 */

#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/ble.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "ble_ops.h"

/* ---------- Radio-side ops ---------- */

static alp_status_t sw_open(alp_ble_radio_state_t *st,
                            alp_capabilities_t *caps_out)
{
    st->be_data = NULL;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t sw_advertise_start(alp_ble_radio_state_t *st,
                                       const alp_ble_adv_config_t *cfg)
{
    (void)st;
    (void)cfg;
    return ALP_OK;
}

static alp_status_t sw_advertise_stop(alp_ble_radio_state_t *st)
{
    (void)st;
    return ALP_OK;
}

static alp_status_t sw_gatt_register_service(alp_ble_radio_state_t *st,
                                             const alp_ble_service_def_t *def,
                                             alp_ble_attr_handle_t *handles_out)
{
    (void)st;
    (void)def;
    (void)handles_out;
    return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t sw_gatt_notify(alp_ble_radio_state_t *radio_st,
                                   alp_ble_conn_state_t *conn_st,
                                   alp_ble_attr_handle_t handle,
                                   const uint8_t *payload, size_t len)
{
    (void)radio_st;
    (void)conn_st;
    (void)handle;
    (void)payload;
    (void)len;
    return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t sw_scan_start(alp_ble_radio_state_t *st, bool active,
                                  alp_ble_scan_cb_t cb, void *user)
{
    (void)st;
    (void)active;
    (void)cb;
    (void)user;
    return ALP_OK;
}

static alp_status_t sw_scan_stop(alp_ble_radio_state_t *st)
{
    (void)st;
    return ALP_OK;
}

static alp_status_t sw_connect(alp_ble_radio_state_t *st,
                               const alp_ble_addr_t *peer,
                               uint32_t timeout_ms,
                               alp_ble_conn_state_t *conn_st)
{
    (void)st;
    (void)peer;
    (void)timeout_ms;
    (void)conn_st;
    /* No real controller, so no real peer can connect.  Returning
     * NOT_IMPLEMENTED rather than OK keeps the dispatcher from
     * handing out a conn handle that no subsequent op can use. */
    return ALP_ERR_NOT_IMPLEMENTED;
}

static void sw_close(alp_ble_radio_state_t *st)
{
    (void)st;
}

/* ---------- Conn-side ops ---------- */

static alp_status_t sw_disconnect(alp_ble_conn_state_t *conn_st)
{
    (void)conn_st;
    return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t sw_gatt_read(alp_ble_conn_state_t *conn_st,
                                 alp_ble_attr_handle_t handle,
                                 uint8_t *out, size_t out_cap,
                                 size_t *out_len, uint32_t timeout_ms)
{
    (void)conn_st;
    (void)handle;
    (void)out;
    (void)out_cap;
    (void)timeout_ms;
    if (out_len != NULL) *out_len = 0;
    return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t sw_gatt_write(alp_ble_conn_state_t *conn_st,
                                  alp_ble_attr_handle_t handle,
                                  const uint8_t *data, size_t len,
                                  uint32_t timeout_ms)
{
    (void)conn_st;
    (void)handle;
    (void)data;
    (void)len;
    (void)timeout_ms;
    return ALP_ERR_NOT_IMPLEMENTED;
}

/* ---------- Registration ---------- */

static const alp_ble_ops_t _ops = {
    .open                  = sw_open,
    .advertise_start       = sw_advertise_start,
    .advertise_stop        = sw_advertise_stop,
    .gatt_register_service = sw_gatt_register_service,
    .gatt_notify           = sw_gatt_notify,
    .scan_start            = sw_scan_start,
    .scan_stop             = sw_scan_stop,
    .connect               = sw_connect,
    .close                 = sw_close,
    .disconnect            = sw_disconnect,
    .gatt_read             = sw_gatt_read,
    .gatt_write            = sw_gatt_write,
};

ALP_BACKEND_REGISTER(ble, sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
