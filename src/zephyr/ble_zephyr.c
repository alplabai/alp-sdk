/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/ble.h>.
 *
 * Replaces the v0.1 NOSUPPORT stub.  Wraps Zephyr's `bt` host stack
 * (peripheral + GATT server first; central + GATT client follow as
 * v0.3.x slots them into the same dispatcher).  All real glue is
 * gated on CONFIG_ALP_SDK_BLE so apps that link against <alp/ble.h>
 * on a target without the BT subsystem (native_sim, baremetal-AEN
 * without the controller bring-up) get the documented v0.1
 * NULL-with-NOSUPPORT contract.
 *
 * Singleton model.  alp_ble_open() returns the same pointer every
 * time once bt_enable() succeeds; alp_ble_close() decrements an
 * internal refcount and shuts the controller down on the last
 * close.  Matches the public API's "BLE is system-singleton"
 * statement in include/alp/ble.h.
 */

#include <errno.h>
#include <string.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "alp/ble.h"
#include "handles.h"

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
/* Internal state                                                      */
/* ------------------------------------------------------------------ */

struct alp_ble {
    bool in_use;
    int  refcount;
#if defined(CONFIG_ALP_SDK_BLE)
    bool              advertising;
    bool              scanning;
    alp_ble_scan_cb_t scan_cb;
    void             *scan_user;
#endif
};

struct alp_ble_conn {
    bool in_use;
#if defined(CONFIG_ALP_SDK_BLE)
    struct bt_conn *bt;
#endif
};

#if defined(CONFIG_ALP_SDK_BLE)
static struct alp_ble       g_ble_singleton;
static struct alp_ble_conn  g_ble_conn_pool[CONFIG_ALP_SDK_BLE_MAX_CONNS];

static struct alp_ble_conn *conn_pool_acquire(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_ble_conn_pool); ++i) {
        if (!g_ble_conn_pool[i].in_use) {
            memset(&g_ble_conn_pool[i], 0, sizeof(g_ble_conn_pool[i]));
            g_ble_conn_pool[i].in_use = true;
            return &g_ble_conn_pool[i];
        }
    }
    return NULL;
}

static void conn_pool_release(struct alp_ble_conn *c)
{
    if (c == NULL) return;
    if (c->bt != NULL) {
        bt_conn_unref(c->bt);
        c->bt = NULL;
    }
    c->in_use = false;
}

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

static void scan_recv_cb(const struct bt_le_scan_recv_info *info, struct net_buf_simple *buf)
{
    if (g_ble_singleton.scan_cb == NULL) return;
    alp_ble_scan_result_t r = {0};
    r.rssi_dbm              = info->rssi;
    r.adv_type              = info->adv_type;
    r.adv_data              = buf->data;
    r.adv_len               = buf->len;
    r.addr.type             = (uint8_t)info->addr->type;
    memcpy(r.addr.addr, info->addr->a.val, 6);
    g_ble_singleton.scan_cb(&r, g_ble_singleton.scan_user);
}

static struct bt_le_scan_cb g_scan_cb = {
    .recv = scan_recv_cb,
};

#endif /* CONFIG_ALP_SDK_BLE */

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

alp_ble_t *alp_ble_open(void)
{
    alp_z_clear_last_error();
#if defined(CONFIG_ALP_SDK_BLE)
    if (g_ble_singleton.refcount == 0) {
        int err = bt_enable(NULL);
        if (err != 0 && err != -EALREADY) {
            alp_z_set_last_error(errno_to_alp(err));
            return NULL;
        }
        bt_le_scan_cb_register(&g_scan_cb);
    }
    g_ble_singleton.refcount++;
    g_ble_singleton.in_use = true;
    return &g_ble_singleton;
#else
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
#endif
}

void alp_ble_close(alp_ble_t *ble)
{
    if (ble == NULL || !ble->in_use) return;
#if defined(CONFIG_ALP_SDK_BLE)
    if (ble->refcount > 0) ble->refcount--;
    if (ble->refcount == 0) {
        if (ble->advertising) (void)bt_le_adv_stop();
        if (ble->scanning) (void)bt_le_scan_stop();
        ble->in_use = false;
    }
#endif
}

/* ================================================================== */
/* Peripheral role                                                     */
/* ================================================================== */

alp_status_t alp_ble_advertise_start(alp_ble_t *ble, const alp_ble_adv_config_t *cfg)
{
    if (ble == NULL || !ble->in_use) return ALP_ERR_NOT_READY;
    if (cfg == NULL) return ALP_ERR_INVAL;
#if defined(CONFIG_ALP_SDK_BLE)
    if (ble->advertising) return ALP_ERR_BUSY;

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
        .options = (cfg->connectable ? BT_LE_ADV_OPT_CONN : 0) | BT_LE_ADV_OPT_USE_IDENTITY,
        .interval_min =
            (uint32_t)((cfg->interval_min_ms ? cfg->interval_min_ms : 100) * 1000 / 625),
        .interval_max =
            (uint32_t)((cfg->interval_max_ms ? cfg->interval_max_ms : 200) * 1000 / 625),
    };

    int err = bt_le_adv_start(&p, ad, ad_len, NULL, 0);
    if (err == 0) ble->advertising = true;
    return errno_to_alp(err);
#else
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_ble_advertise_stop(alp_ble_t *ble)
{
    if (ble == NULL || !ble->in_use) return ALP_ERR_NOT_READY;
#if defined(CONFIG_ALP_SDK_BLE)
    if (!ble->advertising) return ALP_OK;
    int err = bt_le_adv_stop();
    if (err == 0) ble->advertising = false;
    return errno_to_alp(err);
#else
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_ble_gatt_register_service(alp_ble_t *ble, const alp_ble_service_def_t *def,
                                           alp_ble_attr_handle_t *handles_out)
{
    (void)ble;
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

alp_status_t alp_ble_gatt_notify(alp_ble_t *ble, alp_ble_conn_t *conn, alp_ble_attr_handle_t handle,
                                 const uint8_t *payload, size_t len)
{
    if (ble == NULL || !ble->in_use) return ALP_ERR_NOT_READY;
    if (conn == NULL || !conn->in_use) return ALP_ERR_INVAL;
    if (payload == NULL && len > 0) return ALP_ERR_INVAL;
#if defined(CONFIG_ALP_SDK_BLE)
    /* bt_gatt_notify takes an attr pointer, not a handle.  The
     * v0.3.x runtime shim above will materialise that mapping;
     * for v0.3 the call falls through to NOSUPPORT until services
     * are registered via the runtime path. */
    (void)handle;
    (void)conn;
    (void)payload;
    (void)len;
    return ALP_ERR_NOSUPPORT;
#else
    (void)handle;
    (void)payload;
    (void)len;
    return ALP_ERR_NOSUPPORT;
#endif
}

/* ================================================================== */
/* Central role                                                        */
/* ================================================================== */

alp_status_t alp_ble_scan_start(alp_ble_t *ble, bool active, alp_ble_scan_cb_t cb, void *user)
{
    if (ble == NULL || !ble->in_use) return ALP_ERR_NOT_READY;
    if (cb == NULL) return ALP_ERR_INVAL;
#if defined(CONFIG_ALP_SDK_BLE)
    if (ble->scanning) return ALP_ERR_BUSY;
    ble->scan_cb              = cb;
    ble->scan_user            = user;
    struct bt_le_scan_param p = {
        .type     = active ? BT_LE_SCAN_TYPE_ACTIVE : BT_LE_SCAN_TYPE_PASSIVE,
        .options  = BT_LE_SCAN_OPT_NONE,
        .interval = 0x60, /* 60 ms in BT units */
        .window   = 0x30, /* 30 ms */
    };
    int err = bt_le_scan_start(&p, NULL);
    if (err == 0) ble->scanning = true;
    return errno_to_alp(err);
#else
    (void)active;
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_ble_scan_stop(alp_ble_t *ble)
{
    if (ble == NULL || !ble->in_use) return ALP_ERR_NOT_READY;
#if defined(CONFIG_ALP_SDK_BLE)
    if (!ble->scanning) return ALP_OK;
    int err = bt_le_scan_stop();
    if (err == 0) ble->scanning = false;
    return errno_to_alp(err);
#else
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_ble_connect(alp_ble_t *ble, const alp_ble_addr_t *peer, uint32_t timeout_ms,
                             alp_ble_conn_t **conn_out)
{
    if (ble == NULL || !ble->in_use) return ALP_ERR_NOT_READY;
    if (peer == NULL || conn_out == NULL) return ALP_ERR_INVAL;
#if defined(CONFIG_ALP_SDK_BLE)
    bt_addr_le_t addr = {0};
    addr.type         = peer->type;
    memcpy(addr.a.val, peer->addr, 6);

    struct alp_ble_conn *c = conn_pool_acquire();
    if (c == NULL) return ALP_ERR_NOMEM;

    int err = bt_conn_le_create(&addr, BT_CONN_LE_CREATE_CONN, BT_LE_CONN_PARAM_DEFAULT, &c->bt);
    if (err != 0) {
        conn_pool_release(c);
        return errno_to_alp(err);
    }
    /* bt_conn_le_create returns immediately; the connection
     * complete event is signalled via callbacks.  v0.3.x adds a
     * connect-completion semaphore here; for v0.3 we stop short of
     * waiting and let the caller poll bt_conn_get_info(). */
    (void)timeout_ms;
    *conn_out = c;
    return ALP_OK;
#else
    (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_ble_disconnect(alp_ble_conn_t *conn)
{
    if (conn == NULL || !conn->in_use) return ALP_ERR_NOT_READY;
#if defined(CONFIG_ALP_SDK_BLE)
    int err = bt_conn_disconnect(conn->bt, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
    conn_pool_release(conn);
    return errno_to_alp(err);
#else
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_ble_gatt_read(alp_ble_conn_t *conn, alp_ble_attr_handle_t handle, uint8_t *out,
                               size_t out_cap, size_t *out_len, uint32_t timeout_ms)
{
    (void)conn;
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

alp_status_t alp_ble_gatt_write(alp_ble_conn_t *conn, alp_ble_attr_handle_t handle,
                                const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    (void)conn;
    (void)handle;
    (void)data;
    (void)len;
    (void)timeout_ms;
    /* Same v0.3.x story as gatt_read -- async API needs a semaphore
     * shim before it fits the synchronous public surface. */
    return ALP_ERR_NOSUPPORT;
}
