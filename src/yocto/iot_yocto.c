/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Linux userspace MQTT backend for <alp/iot.h>'s alp_mqtt_* surface.
 *
 * Binds against libmosquitto -- the C client library that ships with
 * Eclipse Mosquitto.  Built only when CMake's pkg_check_modules finds
 * `libmosquitto` (gated through src/yocto/CMakeLists.txt); apps
 * targeting Yocto without libmosquitto on the sysroot keep the
 * NOSUPPORT stubs from src/common/stub_backend.c via the
 * ALP_VENDOR_OVERRIDES_MQTT macro.
 *
 * The Wi-Fi half of <alp/iot.h> stays stubbed on Linux: Wi-Fi
 * bring-up on real Yocto images is a system-config concern
 * (wpa_supplicant / NetworkManager) rather than an SDK-side one --
 * customers configure the radio out of band and the SDK consumes
 * the resulting network through plain sockets.
 *
 * Connection model
 * ----------------
 * Caller-driven loop.  alp_mqtt_loop(handle, timeout_ms) pumps the
 * underlying mosquitto event machine for at most `timeout_ms`
 * before returning.  No background thread; apps that want async
 * delivery wrap alp_mqtt_loop in their own pthread.
 *
 * Subscription dispatch
 * ---------------------
 * libmosquitto's on_message callback fires once per inbound message,
 * regardless of filter.  We keep a small per-handle table of
 * `(filter, cb, user)` tuples; on_message walks the table and
 * dispatches to every callback whose filter matches the message's
 * topic via `mosquitto_topic_matches_sub`.  No external locking is
 * needed -- libmosquitto serialises callbacks for a single client
 * instance and the only thread touching the handle is the one
 * calling alp_mqtt_loop.
 *
 * TLS
 * ---
 * `mqtts://` URIs route through libmosquitto's `mosquitto_tls_set`
 * (OpenSSL underneath on a stock Yocto image).  CA / client cert /
 * client key paths come from `alp_mqtt_config_t.tls`; a NULL tls
 * pointer falls back to the host OS's default CA path
 * (`/etc/ssl/certs` on Debian/Ubuntu/Yocto) and no client cert.
 * Production deployments pin `tls->ca_file` to a known-good CA
 * bundle so the broker's identity isn't trustable to any root the
 * host happens to know about.  The `tls->insecure` flag skips peer
 * verification entirely -- development-only; production builds
 * should refuse to ship with it on.
 *
 * Compiled only on Linux hosts/targets, only when libmosquitto is
 * present on the sysroot.
 */

#if !defined(__linux__)
#error "iot_yocto.c (yocto backend) requires a Linux target"
#endif

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mosquitto.h>

#include "alp/iot.h"
#include "alp/peripheral.h"
#include "alp_internal.h"

#ifndef ALP_SDK_YOCTO_MAX_MQTT_HANDLES
#define ALP_SDK_YOCTO_MAX_MQTT_HANDLES 2
#endif

#ifndef ALP_SDK_YOCTO_MAX_MQTT_SUBS
#define ALP_SDK_YOCTO_MAX_MQTT_SUBS 8
#endif

#ifndef ALP_SDK_YOCTO_MQTT_HOST_MAX
#define ALP_SDK_YOCTO_MQTT_HOST_MAX 128
#endif

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

struct mqtt_sub {
    char             *filter;
    alp_mqtt_msg_cb_t cb;
    void             *user;
};

struct alp_mqtt {
    bool              in_use;
    struct mosquitto *mosq;
    char              host[ALP_SDK_YOCTO_MQTT_HOST_MAX];
    int               port;
    uint16_t          keepalive_s;
    bool              use_tls;
    bool              connected;
    struct mqtt_sub   subs[ALP_SDK_YOCTO_MAX_MQTT_SUBS];
    size_t            nsubs;
};

/* Default system CA path on Yocto / Debian / Ubuntu.  Overridden by
 * tls->ca_file when the caller supplies it.  Picked at compile time
 * rather than runtime-discovered: every E1M Yocto image uses
 * openssl-misc's default layout. */
#ifndef ALP_SDK_YOCTO_DEFAULT_CA_PATH
#define ALP_SDK_YOCTO_DEFAULT_CA_PATH "/etc/ssl/certs"
#endif

static struct alp_mqtt g_mqtt_pool[ALP_SDK_YOCTO_MAX_MQTT_HANDLES];

/* libmosquitto requires a one-time global init.  Initialise lazily
 * on first open() so apps that never touch MQTT don't pay the cost;
 * cleanup is intentionally skipped (it's mostly bookkeeping NOPs
 * and process exit reaps the rest). */
static bool             g_mosq_lib_init_done;

static struct alp_mqtt *pool_acquire(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_mqtt_pool); ++i) {
        if (!g_mqtt_pool[i].in_use) {
            memset(&g_mqtt_pool[i], 0, sizeof(g_mqtt_pool[i]));
            g_mqtt_pool[i].in_use = true;
            return &g_mqtt_pool[i];
        }
    }
    return NULL;
}

static void pool_release(struct alp_mqtt *h)
{
    if (h == NULL) {
        return;
    }
    if (h->mosq != NULL) {
        if (h->connected) {
            (void)mosquitto_disconnect(h->mosq);
        }
        mosquitto_destroy(h->mosq);
        h->mosq = NULL;
    }
    for (size_t i = 0; i < h->nsubs; ++i) {
        free(h->subs[i].filter);
        h->subs[i].filter = NULL;
    }
    h->nsubs  = 0;
    h->in_use = false;
}

static alp_status_t mosq_to_alp(int rc)
{
    switch (rc) {
    case MOSQ_ERR_SUCCESS:
        return ALP_OK;
    case MOSQ_ERR_INVAL:
        return ALP_ERR_INVAL;
    case MOSQ_ERR_NOMEM:
        return ALP_ERR_NOMEM;
    case MOSQ_ERR_PROTOCOL:
    case MOSQ_ERR_NOT_SUPPORTED:
        return ALP_ERR_NOSUPPORT;
    case MOSQ_ERR_NO_CONN:
    case MOSQ_ERR_CONN_LOST:
    case MOSQ_ERR_CONN_REFUSED:
        return ALP_ERR_NOT_READY;
    case MOSQ_ERR_AUTH:
        return ALP_ERR_INVAL;
    default:
        return ALP_ERR_IO;
    }
}

/* Parse the broker URI into the handle's host / port / use_tls
 * fields.  Accepts:
 *   - "mqtt://host[:port]"   (default port 1883)
 *   - "mqtts://host[:port]"  (default port 8883)
 * Returns ALP_OK on success; ALP_ERR_INVAL on malformed input. */
static alp_status_t parse_broker_uri(struct alp_mqtt *h, const char *uri)
{
    if (uri == NULL) {
        return ALP_ERR_INVAL;
    }
    const char *rest        = NULL;
    int         default_port;
    if (strncmp(uri, "mqtts://", 8) == 0) {
        rest         = uri + 8;
        default_port = 8883;
        h->use_tls   = true;
    } else if (strncmp(uri, "mqtt://", 7) == 0) {
        rest         = uri + 7;
        default_port = 1883;
        h->use_tls   = false;
    } else {
        return ALP_ERR_INVAL;
    }
    size_t rlen = strlen(rest);
    if (rlen == 0 || rlen >= sizeof(h->host)) {
        return ALP_ERR_INVAL;
    }
    memcpy(h->host, rest, rlen + 1);
    h->port = default_port;

    char *colon = strchr(h->host, ':');
    if (colon != NULL) {
        *colon        = '\0';
        const char *p = colon + 1;
        if (*p == '\0') {
            return ALP_ERR_INVAL;
        }
        long parsed = strtol(p, NULL, 10);
        if (parsed <= 0 || parsed > 65535) {
            return ALP_ERR_INVAL;
        }
        h->port = (int)parsed;
    }
    if (h->host[0] == '\0') {
        return ALP_ERR_INVAL;
    }
    return ALP_OK;
}

/* Apply TLS parameters to the freshly-created mosquitto handle.
 * Returns ALP_OK on success.  ALP_ERR_IO if mosquitto_tls_set fails
 * (typically because the CA file doesn't exist or isn't a valid
 * PEM).  Skipped when the URI scheme is "mqtt://". */
static alp_status_t apply_tls(struct alp_mqtt *h, const alp_mqtt_tls_config_t *tls)
{
    if (!h->use_tls) {
        return ALP_OK;
    }
    const char *cafile   = (tls != NULL) ? tls->ca_file   : NULL;
    const char *certfile = (tls != NULL) ? tls->cert_file : NULL;
    const char *keyfile  = (tls != NULL) ? tls->key_file  : NULL;
    /* mosquitto_tls_set requires at least one of (cafile, capath)
     * for peer verification.  Fall through to the system CA dir
     * when the caller doesn't pin a specific bundle. */
    const char *capath = (cafile == NULL) ? ALP_SDK_YOCTO_DEFAULT_CA_PATH : NULL;
    int rc = mosquitto_tls_set(h->mosq, cafile, capath, certfile, keyfile, NULL);
    if (rc != MOSQ_ERR_SUCCESS) {
        return mosq_to_alp(rc);
    }
    if (tls != NULL && tls->insecure) {
        /* mosquitto_tls_insecure_set must be called *after*
         * mosquitto_tls_set; documented behavior. */
        rc = mosquitto_tls_insecure_set(h->mosq, true);
        if (rc != MOSQ_ERR_SUCCESS) {
            return mosq_to_alp(rc);
        }
    }
    return ALP_OK;
}

static int qos_to_int(alp_mqtt_qos_t q)
{
    switch (q) {
    case ALP_MQTT_QOS_1:
        return 1;
    case ALP_MQTT_QOS_2:
        return 2;
    case ALP_MQTT_QOS_0:
    default:
        return 0;
    }
}

static void on_message(struct mosquitto *mosq, void *obj, const struct mosquitto_message *msg)
{
    (void)mosq;
    struct alp_mqtt *h = (struct alp_mqtt *)obj;
    if (h == NULL || msg == NULL || msg->topic == NULL) {
        return;
    }
    for (size_t i = 0; i < h->nsubs; ++i) {
        bool match = false;
        int  rc    = mosquitto_topic_matches_sub(h->subs[i].filter, msg->topic, &match);
        if (rc != MOSQ_ERR_SUCCESS) {
            continue;
        }
        if (match && h->subs[i].cb != NULL) {
            h->subs[i].cb(msg->topic, (const uint8_t *)msg->payload, (size_t)msg->payloadlen,
                          h->subs[i].user);
        }
    }
}

static void on_connect(struct mosquitto *mosq, void *obj, int rc)
{
    (void)mosq;
    struct alp_mqtt *h = (struct alp_mqtt *)obj;
    if (h != NULL) {
        h->connected = (rc == 0);
    }
}

static void on_disconnect(struct mosquitto *mosq, void *obj, int rc)
{
    (void)mosq;
    (void)rc;
    struct alp_mqtt *h = (struct alp_mqtt *)obj;
    if (h != NULL) {
        h->connected = false;
    }
}

alp_mqtt_t *alp_mqtt_open(const alp_mqtt_config_t *cfg)
{
    if (cfg == NULL || cfg->broker_uri == NULL) {
        alp_internal_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

    if (!g_mosq_lib_init_done) {
        if (mosquitto_lib_init() != MOSQ_ERR_SUCCESS) {
            alp_internal_set_last_error(ALP_ERR_IO);
            return NULL;
        }
        g_mosq_lib_init_done = true;
    }

    struct alp_mqtt *h = pool_acquire();
    if (h == NULL) {
        alp_internal_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    alp_status_t rc = parse_broker_uri(h, cfg->broker_uri);
    if (rc != ALP_OK) {
        alp_internal_set_last_error(rc);
        pool_release(h);
        return NULL;
    }
    h->keepalive_s = (cfg->keepalive_s != 0) ? cfg->keepalive_s : 60;

    h->mosq        = mosquitto_new(cfg->client_id, cfg->clean_session, h);
    if (h->mosq == NULL) {
        alp_internal_set_last_error(errno == ENOMEM ? ALP_ERR_NOMEM : ALP_ERR_INVAL);
        pool_release(h);
        return NULL;
    }
    if (cfg->username != NULL) {
        int mrc = mosquitto_username_pw_set(h->mosq, cfg->username, cfg->password);
        if (mrc != MOSQ_ERR_SUCCESS) {
            alp_internal_set_last_error(mosq_to_alp(mrc));
            pool_release(h);
            return NULL;
        }
    }
    /* TLS hookup happens before the callback wiring so a TLS config
     * error surfaces here rather than later at connect() time when
     * it's harder to attribute to a misconfigured CA bundle. */
    alp_status_t tls_rc = apply_tls(h, cfg->tls);
    if (tls_rc != ALP_OK) {
        alp_internal_set_last_error(tls_rc);
        pool_release(h);
        return NULL;
    }
    mosquitto_message_callback_set(h->mosq, on_message);
    mosquitto_connect_callback_set(h->mosq, on_connect);
    mosquitto_disconnect_callback_set(h->mosq, on_disconnect);
    return h;
}

alp_status_t alp_mqtt_connect(alp_mqtt_t *m, uint32_t timeout_ms)
{
    if (m == NULL || !m->in_use || m->mosq == NULL) {
        return ALP_ERR_INVAL;
    }
    /* mosquitto_connect is synchronous-by-default (handshakes the
     * TCP + MQTT CONNECT/CONNACK exchange before returning) and
     * already honours network timeouts through the underlying
     * socket layer.  The caller's timeout_ms acts as a guard for
     * the post-connect handshake -- we drive one mosquitto_loop
     * pass with it so the on_connect callback flips h->connected
     * before this function returns. */
    int rc = mosquitto_connect(m->mosq, m->host, m->port, m->keepalive_s);
    if (rc != MOSQ_ERR_SUCCESS) {
        return mosq_to_alp(rc);
    }
    /* Drive the loop once so the CONNACK callback fires. */
    rc = mosquitto_loop(m->mosq, (int)timeout_ms, 1);
    if (rc != MOSQ_ERR_SUCCESS) {
        return mosq_to_alp(rc);
    }
    return m->connected ? ALP_OK : ALP_ERR_NOT_READY;
}

alp_status_t alp_mqtt_publish(alp_mqtt_t *m, const char *topic, const uint8_t *payload, size_t len,
                              alp_mqtt_qos_t qos, bool retain)
{
    if (m == NULL || !m->in_use || m->mosq == NULL || topic == NULL) {
        return ALP_ERR_INVAL;
    }
    if (len > INT_MAX) {
        return ALP_ERR_INVAL;
    }
    int rc = mosquitto_publish(m->mosq, NULL, topic, (int)len, payload, qos_to_int(qos), retain);
    if (rc != MOSQ_ERR_SUCCESS) {
        return mosq_to_alp(rc);
    }
    return ALP_OK;
}

alp_status_t alp_mqtt_subscribe(alp_mqtt_t *m, const char *topic_filter, alp_mqtt_qos_t qos,
                                alp_mqtt_msg_cb_t cb, void *user)
{
    if (m == NULL || !m->in_use || m->mosq == NULL || topic_filter == NULL || cb == NULL) {
        return ALP_ERR_INVAL;
    }
    if (m->nsubs >= ARRAY_SIZE(m->subs)) {
        return ALP_ERR_NOMEM;
    }
    char *filter_copy = strdup(topic_filter);
    if (filter_copy == NULL) {
        return ALP_ERR_NOMEM;
    }
    int rc = mosquitto_subscribe(m->mosq, NULL, topic_filter, qos_to_int(qos));
    if (rc != MOSQ_ERR_SUCCESS) {
        free(filter_copy);
        return mosq_to_alp(rc);
    }
    m->subs[m->nsubs].filter = filter_copy;
    m->subs[m->nsubs].cb     = cb;
    m->subs[m->nsubs].user   = user;
    ++m->nsubs;
    return ALP_OK;
}

alp_status_t alp_mqtt_loop(alp_mqtt_t *m, uint32_t timeout_ms)
{
    if (m == NULL || !m->in_use || m->mosq == NULL) {
        return ALP_ERR_INVAL;
    }
    /* mosquitto_loop's timeout is int-typed; clamp at INT_MAX so a
     * caller passing UINT32_MAX-as-forever doesn't roll over. */
    int t  = (timeout_ms > (uint32_t)INT_MAX) ? INT_MAX : (int)timeout_ms;
    int rc = mosquitto_loop(m->mosq, t, 1);
    if (rc != MOSQ_ERR_SUCCESS) {
        return mosq_to_alp(rc);
    }
    return ALP_OK;
}

void alp_mqtt_close(alp_mqtt_t *m)
{
    pool_release(m);
}
