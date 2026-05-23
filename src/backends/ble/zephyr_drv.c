/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr BT-host backend for the <alp/ble.h> surface.
 * Registers as silicon_ref="*" at priority 100 -- mirrors the
 * design spec Section 2 backend matrix (zephyr_drv wins on every
 * SoC unless a more specific backend registers).
 *
 * V2N CC3501E note: the CC3501E Wi-Fi 6 + BLE 5.4 coprocessor on
 * the AEN SoM is wired into Zephyr's DT as a
 * tilab,cc3501-bluetooth-class node so the BT host subsystem
 * dispatches HCI through TI's controller-side driver transparently;
 * the Zephyr backend handles V2N without a separate registry entry.
 *
 * Gated on CONFIG_ALP_SDK_BLE -- when OFF the I/O ops return
 * NOSUPPORT but the registry entry still links so the dispatcher
 * picks it ahead of sw_fallback on real silicon builds with BLE in
 * the device tree.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/ble.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "ble_ops.h"

#if defined(CONFIG_ALP_SDK_BLE)
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#endif

#ifndef CONFIG_ALP_SDK_BLE_MAX_CONNS
#define CONFIG_ALP_SDK_BLE_MAX_CONNS 2
#endif

/* ------------------------------------------------------------------ */
/* Backend-owned per-handle state                                      */
/* ------------------------------------------------------------------ */

struct ble_radio_be {
    int  refcount;
#if defined(CONFIG_ALP_SDK_BLE)
    bool              advertising;
    bool              scanning;
    alp_ble_scan_cb_t scan_cb;
    void             *scan_user;
#endif
};

struct ble_conn_be {
#if defined(CONFIG_ALP_SDK_BLE)
    struct bt_conn *bt;
#else
    int unused;
#endif
};

static struct ble_radio_be _radio_be;
static struct ble_conn_be  _conn_be_pool[CONFIG_ALP_SDK_BLE_MAX_CONNS];
static bool                _conn_be_in_use[CONFIG_ALP_SDK_BLE_MAX_CONNS];

static struct ble_conn_be *_conn_be_alloc(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(_conn_be_pool); ++i) {
        if (!_conn_be_in_use[i]) {
            memset(&_conn_be_pool[i], 0, sizeof(_conn_be_pool[i]));
            _conn_be_in_use[i] = true;
            return &_conn_be_pool[i];
        }
    }
    return NULL;
}

static void _conn_be_free(struct ble_conn_be *p)
{
    if (p == NULL) return;
#if defined(CONFIG_ALP_SDK_BLE)
    if (p->bt != NULL) {
        bt_conn_unref(p->bt);
        p->bt = NULL;
    }
#endif
    for (size_t i = 0; i < ARRAY_SIZE(_conn_be_pool); ++i) {
        if (&_conn_be_pool[i] == p) {
            _conn_be_in_use[i] = false;
            return;
        }
    }
}

#if defined(CONFIG_ALP_SDK_BLE)
static alp_status_t errno_to_alp(int err)
{
    switch (err) {
    case 0:
        return ALP_OK;
    case -EINVAL:
        return ALP_ERR_INVAL;
    case -EBUSY:
        return ALP_ERR_BUSY;
    case -EAGAIN:
    case -ETIMEDOUT:
        return ALP_ERR_TIMEOUT;
    case -EIO:
        return ALP_ERR_IO;
    case -ENOTSUP:
    case -ENOSYS:
        return ALP_ERR_NOSUPPORT;
    case -ENOMEM:
        return ALP_ERR_NOMEM;
    default:
        return ALP_ERR_IO;
    }
}

static void scan_recv_cb(const struct bt_le_scan_recv_info *info,
                         struct net_buf_simple *buf)
{
    if (_radio_be.scan_cb == NULL) return;
    alp_ble_scan_result_t r = {0};
    r.rssi_dbm              = info->rssi;
    r.adv_type              = info->adv_type;
    r.adv_data              = buf->data;
    r.adv_len               = buf->len;
    r.addr.type             = (uint8_t)info->addr->type;
    memcpy(r.addr.addr, info->addr->a.val, 6);
    _radio_be.scan_cb(&r, _radio_be.scan_user);
}

static struct bt_le_scan_cb _scan_cb = {
    .recv = scan_recv_cb,
};
#endif /* CONFIG_ALP_SDK_BLE */

/* ================================================================== */
/* Radio-side ops                                                      */
/* ================================================================== */

static alp_status_t z_open(alp_ble_radio_state_t *st,
                           alp_capabilities_t *caps_out)
{
#if defined(CONFIG_ALP_SDK_BLE)
    if (_radio_be.refcount == 0) {
        int err = bt_enable(NULL);
        if (err != 0 && err != -EALREADY) {
            caps_out->flags = 0u;
            return errno_to_alp(err);
        }
        bt_le_scan_cb_register(&_scan_cb);
    }
    _radio_be.refcount++;
    st->be_data = &_radio_be;
    caps_out->flags = 0u;
    return ALP_OK;
#else
    (void)st;
    caps_out->flags = 0u;
    return ALP_ERR_NOSUPPORT;
#endif
}

static void z_close(alp_ble_radio_state_t *st)
{
#if defined(CONFIG_ALP_SDK_BLE)
    struct ble_radio_be *be = (struct ble_radio_be *)st->be_data;
    if (be == NULL) return;
    if (be->refcount > 0) be->refcount--;
    if (be->refcount == 0) {
        if (be->advertising) {
            (void)bt_le_adv_stop();
            be->advertising = false;
        }
        if (be->scanning) {
            (void)bt_le_scan_stop();
            be->scanning = false;
        }
    }
    st->be_data = NULL;
#else
    (void)st;
#endif
}

static alp_status_t z_advertise_start(alp_ble_radio_state_t *st,
                                      const alp_ble_adv_config_t *cfg)
{
#if defined(CONFIG_ALP_SDK_BLE)
    struct ble_radio_be *be = (struct ble_radio_be *)st->be_data;
    if (be == NULL) return ALP_ERR_NOT_READY;
    if (be->advertising) return ALP_ERR_BUSY;

    /* Pack adv data: flags + complete local name (if provided) + service UUIDs.
     * Size budget is 31 bytes total per BT spec; we trust the caller
     * not to exceed that for v0.3.  v0.3.x adds extended-advertising
     * support for larger payloads. */
    struct bt_data       ad[3];
    size_t               ad_len = 0;

    static const uint8_t flags  = BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR;
    ad[ad_len++]                = (struct bt_data){
        .type     = BT_DATA_FLAGS,
        .data_len = 1,
        .data     = &flags,
    };
    if (cfg->name != NULL && cfg->name[0] != '\0') {
        ad[ad_len++] = (struct bt_data){
            .type     = BT_DATA_NAME_COMPLETE,
            .data_len = (uint8_t)strlen(cfg->name),
            .data     = (const uint8_t *)cfg->name,
        };
    }
    if (cfg->services != NULL && cfg->num_services > 0) {
        ad[ad_len++] = (struct bt_data){
            .type     = BT_DATA_UUID128_ALL,
            .data_len = (uint8_t)(cfg->num_services * sizeof(alp_ble_uuid_t)),
            .data     = (const uint8_t *)cfg->services,
        };
    }

    struct bt_le_adv_param p = {
        .options = (cfg->connectable ? BT_LE_ADV_OPT_CONN : 0) |
                   BT_LE_ADV_OPT_USE_IDENTITY,
        .interval_min =
            (uint32_t)((cfg->interval_min_ms ? cfg->interval_min_ms : 100) *
                       1000 / 625),
        .interval_max =
            (uint32_t)((cfg->interval_max_ms ? cfg->interval_max_ms : 200) *
                       1000 / 625),
    };

    int err = bt_le_adv_start(&p, ad, ad_len, NULL, 0);
    if (err == 0) be->advertising = true;
    return errno_to_alp(err);
#else
    (void)st;
    (void)cfg;
    return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_advertise_stop(alp_ble_radio_state_t *st)
{
#if defined(CONFIG_ALP_SDK_BLE)
    struct ble_radio_be *be = (struct ble_radio_be *)st->be_data;
    if (be == NULL) return ALP_ERR_NOT_READY;
    if (!be->advertising) return ALP_OK;
    int err = bt_le_adv_stop();
    if (err == 0) be->advertising = false;
    return errno_to_alp(err);
#else
    (void)st;
    return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_gatt_register_service(alp_ble_radio_state_t *st,
                                            const alp_ble_service_def_t *def,
                                            alp_ble_attr_handle_t *handles_out)
{
    (void)st;
    (void)def;
    (void)handles_out;
    /* GATT service registration via Zephyr's BT_GATT_SERVICE_DEFINE
     * is statically declared at compile time -- not runtime-friendly
     * for arbitrary service definitions.  v0.3.x lands a runtime
     * shim using bt_gatt_service_register on a heap-allocated
     * bt_gatt_attr array.  Until then this entry stays NOSUPPORT
     * regardless of CONFIG_ALP_SDK_BLE. */
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t z_gatt_notify(alp_ble_radio_state_t *radio_st,
                                  alp_ble_conn_state_t *conn_st,
                                  alp_ble_attr_handle_t handle,
                                  const uint8_t *payload, size_t len)
{
    (void)radio_st;
    (void)conn_st;
    (void)handle;
    (void)payload;
    (void)len;
    /* bt_gatt_notify takes an attr pointer, not a handle.  The
     * v0.3.x runtime shim above will materialise that mapping;
     * for v0.3 the call falls through to NOSUPPORT until services
     * are registered via the runtime path. */
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t z_scan_start(alp_ble_radio_state_t *st, bool active,
                                 alp_ble_scan_cb_t cb, void *user)
{
#if defined(CONFIG_ALP_SDK_BLE)
    struct ble_radio_be *be = (struct ble_radio_be *)st->be_data;
    if (be == NULL) return ALP_ERR_NOT_READY;
    if (be->scanning) return ALP_ERR_BUSY;
    be->scan_cb              = cb;
    be->scan_user            = user;
    struct bt_le_scan_param p = {
        .type     = active ? BT_LE_SCAN_TYPE_ACTIVE : BT_LE_SCAN_TYPE_PASSIVE,
        .options  = BT_LE_SCAN_OPT_NONE,
        .interval = 0x60, /* 60 ms in BT units */
        .window   = 0x30, /* 30 ms */
    };
    int err = bt_le_scan_start(&p, NULL);
    if (err == 0) be->scanning = true;
    return errno_to_alp(err);
#else
    (void)st;
    (void)active;
    (void)cb;
    (void)user;
    return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_scan_stop(alp_ble_radio_state_t *st)
{
#if defined(CONFIG_ALP_SDK_BLE)
    struct ble_radio_be *be = (struct ble_radio_be *)st->be_data;
    if (be == NULL) return ALP_ERR_NOT_READY;
    if (!be->scanning) return ALP_OK;
    int err = bt_le_scan_stop();
    if (err == 0) be->scanning = false;
    return errno_to_alp(err);
#else
    (void)st;
    return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_connect(alp_ble_radio_state_t *st,
                              const alp_ble_addr_t *peer,
                              uint32_t timeout_ms,
                              alp_ble_conn_state_t *conn_st)
{
#if defined(CONFIG_ALP_SDK_BLE)
    bt_addr_le_t addr = {0};
    addr.type         = peer->type;
    memcpy(addr.a.val, peer->addr, 6);

    struct ble_conn_be *c = _conn_be_alloc();
    if (c == NULL) return ALP_ERR_NOMEM;

    int err = bt_conn_le_create(&addr, BT_CONN_LE_CREATE_CONN,
                                BT_LE_CONN_PARAM_DEFAULT, &c->bt);
    if (err != 0) {
        _conn_be_free(c);
        return errno_to_alp(err);
    }
    /* bt_conn_le_create returns immediately; the connection
     * complete event is signalled via callbacks.  v0.3.x adds a
     * connect-completion semaphore here; for v0.3 we stop short of
     * waiting and let the caller poll bt_conn_get_info(). */
    (void)timeout_ms;
    (void)st;
    conn_st->be_data = c;
    return ALP_OK;
#else
    (void)st;
    (void)peer;
    (void)timeout_ms;
    (void)conn_st;
    return ALP_ERR_NOSUPPORT;
#endif
}

/* ================================================================== */
/* Conn-side ops                                                       */
/* ================================================================== */

static alp_status_t z_disconnect(alp_ble_conn_state_t *conn_st)
{
#if defined(CONFIG_ALP_SDK_BLE)
    struct ble_conn_be *c = (struct ble_conn_be *)conn_st->be_data;
    if (c == NULL) return ALP_ERR_NOT_READY;
    int err = bt_conn_disconnect(c->bt, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    _conn_be_free(c);
    conn_st->be_data = NULL;
    return errno_to_alp(err);
#else
    (void)conn_st;
    return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_gatt_read(alp_ble_conn_state_t *conn_st,
                                alp_ble_attr_handle_t handle,
                                uint8_t *out, size_t out_cap,
                                size_t *out_len, uint32_t timeout_ms)
{
    (void)conn_st;
    (void)handle;
    (void)out;
    (void)out_cap;
    (void)out_len;
    (void)timeout_ms;
    /* GATT discovery + sync read flow lands in v0.3.x once we've
     * added the wait-for-callback helper.  bt_gatt_read is async
     * via bt_gatt_read_func_t; the wrapper needs a semaphore +
     * scratch buffer per call.  Keep NOSUPPORT for now. */
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t z_gatt_write(alp_ble_conn_state_t *conn_st,
                                 alp_ble_attr_handle_t handle,
                                 const uint8_t *data, size_t len,
                                 uint32_t timeout_ms)
{
    (void)conn_st;
    (void)handle;
    (void)data;
    (void)len;
    (void)timeout_ms;
    /* Same v0.3.x story as gatt_read -- async API needs a semaphore
     * shim before it fits the synchronous public surface. */
    return ALP_ERR_NOSUPPORT;
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const alp_ble_ops_t _ops = {
    .open                  = z_open,
    .advertise_start       = z_advertise_start,
    .advertise_stop        = z_advertise_stop,
    .gatt_register_service = z_gatt_register_service,
    .gatt_notify           = z_gatt_notify,
    .scan_start            = z_scan_start,
    .scan_stop             = z_scan_stop,
    .connect               = z_connect,
    .close                 = z_close,
    .disconnect            = z_disconnect,
    .gatt_read             = z_gatt_read,
    .gatt_write            = z_gatt_write,
};

ALP_BACKEND_REGISTER(ble, zephyr_drv, {
    .silicon_ref = "*",
    .vendor      = "zephyr",
    .base_caps   = 0u,
    .priority    = 100,
    .ops         = &_ops,
    .probe       = NULL,
});
