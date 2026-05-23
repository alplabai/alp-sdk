/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE class dispatcher.  Owns the public <alp/ble.h> surface --
 * both the radio singleton (alp_ble_t) and per-connection
 * (alp_ble_conn_t) handles -- on top of the backend registry
 * mechanism shipped in Slice 0 (PR #17).
 *
 * Per design spec Section 4: ONE class registry covers both
 * handle types since the controller is one piece of hardware.
 * The ops vtable carries function pointers for both surfaces; the
 * dispatcher maintains two separate handle pools (radio + conn).
 *
 * Slice 4b ships no vendor extensions for BLE: on V2N the CC3501E
 * proxy is handled inside the Zephyr backend as a
 * tilab,cc3501-bluetooth-class DT node which the BT host subsystem
 * routes transparently; no second registry tier is needed.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/ble.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "backends/ble/ble_ops.h"

ALP_BACKEND_DEFINE_CLASS(ble);

extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#ifndef CONFIG_ALP_SDK_MAX_BLE_HANDLES
#define CONFIG_ALP_SDK_MAX_BLE_HANDLES 1
#endif
#ifndef CONFIG_ALP_SDK_MAX_BLE_CONN_HANDLES
#define CONFIG_ALP_SDK_MAX_BLE_CONN_HANDLES 4
#endif

static struct alp_ble      _radio_pool[CONFIG_ALP_SDK_MAX_BLE_HANDLES];
static struct alp_ble_conn _conn_pool[CONFIG_ALP_SDK_MAX_BLE_CONN_HANDLES];

static struct alp_ble *_alloc_radio(void)
{
    for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_BLE_HANDLES; ++i) {
        if (!_radio_pool[i].in_use) {
            memset(&_radio_pool[i], 0, sizeof(_radio_pool[i]));
            _radio_pool[i].in_use = true;
            return &_radio_pool[i];
        }
    }
    return NULL;
}

static void _free_radio(struct alp_ble *h) { h->in_use = false; }

static struct alp_ble_conn *_alloc_conn(void)
{
    for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_BLE_CONN_HANDLES; ++i) {
        if (!_conn_pool[i].in_use) {
            memset(&_conn_pool[i], 0, sizeof(_conn_pool[i]));
            _conn_pool[i].in_use = true;
            return &_conn_pool[i];
        }
    }
    return NULL;
}

static void _free_conn(struct alp_ble_conn *h) { h->in_use = false; }

/* ================================================================== */
/* Radio-side dispatch                                                 */
/* ================================================================== */

alp_ble_t *alp_ble_open(void)
{
    alp_z_clear_last_error();
    const alp_backend_t *be = alp_backend_select("ble", ALP_SOC_REF_STR);
    if (be == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
        return NULL;
    }
    const alp_ble_ops_t *ops = (const alp_ble_ops_t *)be->ops;
    if (ops == NULL || ops->open == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
        return NULL;
    }
    struct alp_ble *h = _alloc_radio();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    h->backend = be;
    h->state.ops = ops;
    alp_capabilities_t caps = { .flags = be->base_caps };
    alp_status_t rc = ops->open(&h->state, &caps);
    if (rc != ALP_OK) {
        _free_radio(h);
        alp_z_set_last_error(rc);
        return NULL;
    }
    h->cached_caps = caps;
    return h;
}

void alp_ble_close(alp_ble_t *h)
{
    if (h == NULL || !h->in_use) return;
    if (h->state.ops != NULL && h->state.ops->close != NULL) {
        h->state.ops->close(&h->state);
    }
    _free_radio(h);
}

alp_status_t alp_ble_advertise_start(alp_ble_t *h,
                                     const alp_ble_adv_config_t *cfg)
{
    if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
    if (cfg == NULL) return ALP_ERR_INVAL;
    if (h->state.ops == NULL || h->state.ops->advertise_start == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return h->state.ops->advertise_start(&h->state, cfg);
}

alp_status_t alp_ble_advertise_stop(alp_ble_t *h)
{
    if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
    if (h->state.ops == NULL || h->state.ops->advertise_stop == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return h->state.ops->advertise_stop(&h->state);
}

alp_status_t alp_ble_gatt_register_service(alp_ble_t *h,
                                           const alp_ble_service_def_t *def,
                                           alp_ble_attr_handle_t *handles_out)
{
    if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
    if (def == NULL) return ALP_ERR_INVAL;
    if (h->state.ops == NULL || h->state.ops->gatt_register_service == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return h->state.ops->gatt_register_service(&h->state, def, handles_out);
}

alp_status_t alp_ble_gatt_notify(alp_ble_t *h, alp_ble_conn_t *conn,
                                 alp_ble_attr_handle_t handle,
                                 const uint8_t *payload, size_t len)
{
    if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
    if (conn == NULL || !conn->in_use) return ALP_ERR_INVAL;
    if (payload == NULL && len > 0) return ALP_ERR_INVAL;
    if (h->state.ops == NULL || h->state.ops->gatt_notify == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return h->state.ops->gatt_notify(&h->state, &conn->state,
                                     handle, payload, len);
}

alp_status_t alp_ble_scan_start(alp_ble_t *h, bool active,
                                alp_ble_scan_cb_t cb, void *user)
{
    if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
    if (cb == NULL) return ALP_ERR_INVAL;
    if (h->state.ops == NULL || h->state.ops->scan_start == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return h->state.ops->scan_start(&h->state, active, cb, user);
}

alp_status_t alp_ble_scan_stop(alp_ble_t *h)
{
    if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
    if (h->state.ops == NULL || h->state.ops->scan_stop == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return h->state.ops->scan_stop(&h->state);
}

alp_status_t alp_ble_connect(alp_ble_t *h, const alp_ble_addr_t *peer,
                             uint32_t timeout_ms, alp_ble_conn_t **conn_out)
{
    if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
    if (peer == NULL || conn_out == NULL) return ALP_ERR_INVAL;
    if (h->state.ops == NULL || h->state.ops->connect == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    struct alp_ble_conn *c = _alloc_conn();
    if (c == NULL) return ALP_ERR_NOMEM;
    c->backend     = h->backend;
    c->state.ops   = h->state.ops;
    c->state.radio = h;
    alp_status_t rc = h->state.ops->connect(&h->state, peer, timeout_ms,
                                            &c->state);
    if (rc != ALP_OK) {
        _free_conn(c);
        return rc;
    }
    *conn_out = c;
    return ALP_OK;
}

/* ================================================================== */
/* Conn-side dispatch                                                  */
/* ================================================================== */

alp_status_t alp_ble_disconnect(alp_ble_conn_t *c)
{
    if (c == NULL || !c->in_use) return ALP_ERR_NOT_READY;
    alp_status_t rc = ALP_OK;
    if (c->state.ops != NULL && c->state.ops->disconnect != NULL) {
        rc = c->state.ops->disconnect(&c->state);
    }
    _free_conn(c);
    return rc;
}

alp_status_t alp_ble_gatt_read(alp_ble_conn_t *c, alp_ble_attr_handle_t handle,
                               uint8_t *out, size_t out_cap, size_t *out_len,
                               uint32_t timeout_ms)
{
    if (out_len != NULL) *out_len = 0;
    if (c == NULL || !c->in_use) return ALP_ERR_NOT_READY;
    if (out == NULL && out_cap > 0) return ALP_ERR_INVAL;
    if (c->state.ops == NULL || c->state.ops->gatt_read == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return c->state.ops->gatt_read(&c->state, handle, out, out_cap,
                                   out_len, timeout_ms);
}

alp_status_t alp_ble_gatt_write(alp_ble_conn_t *c, alp_ble_attr_handle_t handle,
                                const uint8_t *data, size_t len,
                                uint32_t timeout_ms)
{
    if (c == NULL || !c->in_use) return ALP_ERR_NOT_READY;
    if (data == NULL && len > 0) return ALP_ERR_INVAL;
    if (c->state.ops == NULL || c->state.ops->gatt_write == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return c->state.ops->gatt_write(&c->state, handle, data, len, timeout_ms);
}

/* ================================================================== */
/* Capability getter                                                   */
/* ================================================================== */

const alp_capabilities_t *alp_ble_capabilities(const alp_ble_t *h)
{
    return (h != NULL) ? &h->cached_caps : NULL;
}
