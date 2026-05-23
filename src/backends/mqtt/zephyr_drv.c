/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Zephyr mqtt_client backend for the <alp/iot.h> MQTT
 * surface.  Lifts the body of src/zephyr/iot_zephyr.c (the legacy
 * v0.2 MQTT wrapper -- the Wi-Fi half migrated earlier on this
 * branch) into a registry-shaped backend.  Registers as
 * silicon_ref="*" at priority 100 -- mirrors the design spec
 * Section 2 backend matrix (zephyr_drv wins on every SoC unless a
 * more specific backend registers).
 *
 * V2N CC3501E note: the CC3501E Wi-Fi 6 + BLE 5.4 coprocessor on
 * the AEN SoM provides the underlying socket stack; the Zephyr mqtt
 * client runs above and is SoM-agnostic, so no separate registry
 * entry is needed for V2N.
 *
 * Gated on CONFIG_ALP_SDK_IOT_MQTT -- when OFF the I/O ops return
 * NOSUPPORT but the registry entry still links so the dispatcher
 * picks it ahead of sw_fallback on real silicon builds with
 * CONFIG_MQTT_LIB + CONFIG_NET_TCP in the device tree.
 *
 * Backend-owned state:
 *   - struct mqtt_be (per-handle; mqtt_client, sockaddr_storage,
 *     rx/tx scratch, topic scratch, msg_cb/user pair, connected flag,
 *     msg-id counter and the client_id / username / password copies
 *     so reconnects survive the customer's source-cfg lifetime).
 *
 * Allocated from a fixed-size per-handle pool (sized by
 * CONFIG_ALP_SDK_MAX_MQTT_HANDLES) and indexed by slot lookup at
 * open / close edges -- the dispatcher's slot pool and this pool
 * have a 1:1 mapping but stay independent so the backend can compile
 * standalone for unit tests.
 */

#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/iot.h>
#include <alp/peripheral.h>

#include "mqtt_ops.h"

#if defined(CONFIG_ALP_SDK_IOT_MQTT)
#include <zephyr/net/mqtt.h>
#include <zephyr/net/socket.h>
#endif

/* ------------------------------------------------------------------ */
/* Pool sizes                                                          */
/* ------------------------------------------------------------------ */

#ifndef CONFIG_ALP_SDK_MAX_MQTT_HANDLES
#define CONFIG_ALP_SDK_MAX_MQTT_HANDLES 2
#endif

/* MQTT scratch buffers per client.  256 B is enough for the kind of
 * JSON payloads the v0.2 reference apps publish (status + a handful
 * of inference results); apps that publish larger blobs override via
 * CONFIG_ALP_SDK_MQTT_BUF_SIZE. */
#ifndef CONFIG_ALP_SDK_MQTT_BUF_SIZE
#define CONFIG_ALP_SDK_MQTT_BUF_SIZE 256
#endif

/* ------------------------------------------------------------------ */
/* Per-handle backend state                                            */
/* ------------------------------------------------------------------ */

#if defined(CONFIG_ALP_SDK_IOT_MQTT)
struct mqtt_be {
    bool                    in_use;
    alp_mqtt_msg_cb_t       msg_cb;
    void                   *msg_user;
    struct mqtt_client      client;
    struct sockaddr_storage broker_addr;
    uint8_t                 rx_buf[CONFIG_ALP_SDK_MQTT_BUF_SIZE];
    uint8_t                 tx_buf[CONFIG_ALP_SDK_MQTT_BUF_SIZE];
    uint8_t                 topic_buf[128]; /* scratch for incoming topic */
    bool                    connected;
    char                    client_id_buf[64];
    char                    username_buf[64];
    char                    password_buf[64];
    struct mqtt_utf8        username_utf8;
    struct mqtt_utf8        password_utf8;
    uint16_t                next_msg_id; /* monotonic, wraps past 0xFFFF */
};

static struct mqtt_be g_mqtt_be_pool[CONFIG_ALP_SDK_MAX_MQTT_HANDLES];

static struct mqtt_be *mqtt_be_acquire(void)
{
    for (size_t i = 0; i < ARRAY_SIZE(g_mqtt_be_pool); ++i) {
        if (!g_mqtt_be_pool[i].in_use) {
            memset(&g_mqtt_be_pool[i], 0, sizeof(g_mqtt_be_pool[i]));
            g_mqtt_be_pool[i].in_use = true;
            return &g_mqtt_be_pool[i];
        }
    }
    return NULL;
}

static void mqtt_be_release(struct mqtt_be *be)
{
    if (be != NULL) be->in_use = false;
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

/* Parse "mqtt(s)?://host[:port]" into host/port/tls.  Returns 0 on
 * success.  No URI-encoding handling -- broker addresses in v0.2 are
 * expected to be plain hostnames or IPs. */
static int parse_broker_uri(const char *uri, char *host_buf, size_t host_buf_len,
                            uint16_t *port_out, bool *tls_out)
{
    if (uri == NULL) return -EINVAL;

    bool        tls    = false;
    const char *cursor = uri;
    if (strncmp(cursor, "mqtts://", 8) == 0) {
        tls = true;
        cursor += 8;
    } else if (strncmp(cursor, "mqtt://", 7) == 0) {
        tls = false;
        cursor += 7;
    } else {
        return -EINVAL;
    }

    /* Default port: 1883 for plain, 8883 for TLS. */
    uint16_t    port  = tls ? 8883 : 1883;

    const char *colon = strrchr(cursor, ':');
    const char *slash = strchr(cursor, '/');
    size_t      host_len;

    if (colon != NULL && (slash == NULL || colon < slash)) {
        host_len    = (size_t)(colon - cursor);
        long parsed = strtol(colon + 1, NULL, 10);
        if (parsed <= 0 || parsed > 65535) return -EINVAL;
        port = (uint16_t)parsed;
    } else if (slash != NULL) {
        host_len = (size_t)(slash - cursor);
    } else {
        host_len = strlen(cursor);
    }

    if (host_len == 0 || host_len >= host_buf_len) return -EINVAL;
    memcpy(host_buf, cursor, host_len);
    host_buf[host_len] = '\0';

    *port_out          = port;
    *tls_out           = tls;
    return 0;
}

static int resolve_broker_addr(const char *host, uint16_t port, struct sockaddr_storage *out)
{
    /* Prefer numeric IPv4 first -- keeps the wrapper resolver-free for
     * the common "broker is a static IP" case.  When CONFIG_DNS_RESOLVER
     * is enabled the caller can pre-resolve via getaddrinfo and pass the
     * numeric form. */
    struct sockaddr_in *sin = (struct sockaddr_in *)out;
    memset(out, 0, sizeof(*out));
    sin->sin_family = AF_INET;
    sin->sin_port   = htons(port);
    if (zsock_inet_pton(AF_INET, host, &sin->sin_addr) == 1) return 0;

#if defined(CONFIG_DNS_RESOLVER)
    struct zsock_addrinfo  hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
    struct zsock_addrinfo *res   = NULL;
    int                    err   = zsock_getaddrinfo(host, NULL, &hints, &res);
    if (err != 0 || res == NULL) {
        if (res != NULL) zsock_freeaddrinfo(res);
        return -EHOSTUNREACH;
    }
    *sin          = *(const struct sockaddr_in *)res->ai_addr;
    sin->sin_port = htons(port);
    zsock_freeaddrinfo(res);
    return 0;
#else
    return -EHOSTUNREACH;
#endif
}

/* Event handler -- called from mqtt_input() in the user's loop
 * thread.  Maps Zephyr MQTT events into the alp surface (CONNACK
 * sets connected=true; PUBLISH dispatches to the user callback). */
static void alp_mqtt_evt_cb(struct mqtt_client *client, const struct mqtt_evt *evt)
{
    struct mqtt_be *be = CONTAINER_OF(client, struct mqtt_be, client);

    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        be->connected = (evt->result == 0);
        break;
    case MQTT_EVT_DISCONNECT:
        be->connected = false;
        break;
    case MQTT_EVT_PUBLISH: {
        const struct mqtt_publish_param *pub = &evt->param.publish;
        if (be->msg_cb == NULL) {
            /* Drop the payload off the wire so the broker doesn't
             * stall on QoS-1+ acknowledgement -- but keep the
             * topic-string and length for callers that subscribed
             * without binding a callback. */
            uint8_t scratch[64];
            size_t  remaining = pub->message.payload.len;
            while (remaining > 0) {
                int n = mqtt_read_publish_payload(client, scratch, MIN(remaining, sizeof(scratch)));
                if (n <= 0) break;
                remaining -= (size_t)n;
            }
            break;
        }

        /* Copy the topic into our scratch buffer so we can NUL-terminate
         * it for the public callback (the wire form is length-delimited). */
        size_t topic_len = MIN(pub->message.topic.topic.size, sizeof(be->topic_buf) - 1);
        memcpy(be->topic_buf, pub->message.topic.topic.utf8, topic_len);
        be->topic_buf[topic_len] = '\0';

        /* Read payload directly into rx_buf -- bounded by buffer size. */
        size_t want = MIN(pub->message.payload.len, sizeof(be->rx_buf));
        size_t got  = 0;
        while (got < want) {
            int n = mqtt_read_publish_payload(client, be->rx_buf + got, want - got);
            if (n <= 0) break;
            got += (size_t)n;
        }

        if (pub->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
            const struct mqtt_puback_param ack = {.message_id = pub->message_id};
            (void)mqtt_publish_qos1_ack(client, &ack);
        }
        be->msg_cb((const char *)be->topic_buf, be->rx_buf, got, be->msg_user);
        break;
    }
    default:
        break;
    }
}

/* Pull the active socket fd out of the mqtt client.  Path differs
 * between non-secure (transport.tcp) and TLS (transport.tls) variants;
 * v0.2 only ships non-secure (TLS lands with security.h in v0.3) but
 * the helper is shaped to extend cleanly. */
static int alp_mqtt_get_fd(struct mqtt_client *c)
{
#if defined(CONFIG_MQTT_LIB_TLS)
    if (c->transport.type == MQTT_TRANSPORT_SECURE) {
        return c->transport.tls.sock;
    }
#endif
    return c->transport.tcp.sock;
}
#endif /* CONFIG_ALP_SDK_IOT_MQTT */

/* ================================================================== */
/* Ops                                                                 */
/* ================================================================== */

static alp_status_t z_open(const alp_mqtt_config_t *cfg,
                           alp_mqtt_backend_state_t *st,
                           alp_capabilities_t *caps_out)
{
#if defined(CONFIG_ALP_SDK_IOT_MQTT)
    struct mqtt_be *be = mqtt_be_acquire();
    if (be == NULL) {
        caps_out->flags = 0u;
        return ALP_ERR_NOMEM;
    }

    char     host[64];
    uint16_t port;
    bool     tls;
    int      err = parse_broker_uri(cfg->broker_uri, host, sizeof(host), &port, &tls);
    if (err != 0) {
        mqtt_be_release(be);
        caps_out->flags = 0u;
        return ALP_ERR_INVAL;
    }

    err = resolve_broker_addr(host, port, &be->broker_addr);
    if (err != 0) {
        mqtt_be_release(be);
        caps_out->flags = 0u;
        return errno_to_alp(err);
    }

    /* Stash the client id locally so we own its memory across
     * reconnects -- the caller's pointer can go out of scope. */
    strncpy(be->client_id_buf, cfg->client_id, sizeof(be->client_id_buf) - 1);

    mqtt_client_init(&be->client);
    be->client.broker         = &be->broker_addr;
    be->client.evt_cb         = alp_mqtt_evt_cb;
    be->client.client_id.utf8 = (uint8_t *)be->client_id_buf;
    be->client.client_id.size = strlen(be->client_id_buf);
    if (cfg->username != NULL) {
        strncpy(be->username_buf, cfg->username, sizeof(be->username_buf) - 1);
        be->username_utf8.utf8 = (uint8_t *)be->username_buf;
        be->username_utf8.size = strlen(be->username_buf);
        be->client.user_name   = &be->username_utf8;
    } else {
        be->client.user_name = NULL;
    }
    if (cfg->password != NULL) {
        strncpy(be->password_buf, cfg->password, sizeof(be->password_buf) - 1);
        be->password_utf8.utf8 = (uint8_t *)be->password_buf;
        be->password_utf8.size = strlen(be->password_buf);
        be->client.password    = &be->password_utf8;
    } else {
        be->client.password = NULL;
    }
    be->client.protocol_version = MQTT_VERSION_3_1_1;
#if defined(CONFIG_MQTT_LIB_TLS)
    be->client.transport.type   = tls ? MQTT_TRANSPORT_SECURE : MQTT_TRANSPORT_NON_SECURE;
#else
    /* TLS Kconfig disabled at build time -- silently downgrade. */
    (void)tls;
    be->client.transport.type   = MQTT_TRANSPORT_NON_SECURE;
#endif
    be->client.rx_buf           = be->rx_buf;
    be->client.rx_buf_size      = sizeof(be->rx_buf);
    be->client.tx_buf           = be->tx_buf;
    be->client.tx_buf_size      = sizeof(be->tx_buf);
    be->client.keepalive        = cfg->keepalive_s;
    be->client.clean_session    = cfg->clean_session ? 1U : 0U;
    be->connected               = false;
    be->next_msg_id             = 1;

    st->be_data     = be;
    caps_out->flags = 0u;
    return ALP_OK;
#else
    (void)cfg;
    (void)st;
    caps_out->flags = 0u;
    return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_connect(alp_mqtt_backend_state_t *st, uint32_t timeout_ms)
{
#if defined(CONFIG_ALP_SDK_IOT_MQTT)
    struct mqtt_be *be = (struct mqtt_be *)st->be_data;
    if (be == NULL) return ALP_ERR_NOT_READY;

    int err = mqtt_connect(&be->client);
    if (err != 0) {
        return errno_to_alp(err);
    }

    /* Pump input until we get CONNACK (which the evt cb sets) or the
     * timeout expires.  poll() with a short slice keeps the wait
     * responsive without busy-spinning. */
    uint32_t deadline = k_uptime_get_32() + timeout_ms;
    while ((int32_t)(deadline - k_uptime_get_32()) > 0) {
        struct zsock_pollfd fds[1] = {0};
        fds[0].fd                  = alp_mqtt_get_fd(&be->client);
        fds[0].events              = ZSOCK_POLLIN;
        int rc                     = zsock_poll(fds, 1, 200);
        if (rc < 0) return errno_to_alp(-errno);
        if (rc > 0) {
            err = mqtt_input(&be->client);
            if (err != 0) return errno_to_alp(err);
        }
        err = mqtt_live(&be->client);
        if (err != 0 && err != -EAGAIN) return errno_to_alp(err);

        if (be->connected) return ALP_OK;
    }
    return ALP_ERR_TIMEOUT;
#else
    (void)st;
    (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_publish(alp_mqtt_backend_state_t *st, const char *topic,
                              const uint8_t *payload, size_t len,
                              alp_mqtt_qos_t qos, bool retain)
{
#if defined(CONFIG_ALP_SDK_IOT_MQTT)
    struct mqtt_be *be = (struct mqtt_be *)st->be_data;
    if (be == NULL) return ALP_ERR_NOT_READY;

    struct mqtt_publish_param p = {0};
    p.message.topic.topic.utf8  = (const uint8_t *)topic;
    p.message.topic.topic.size  = strlen(topic);
    p.message.topic.qos         = (uint8_t)qos;
    p.message.payload.data      = (uint8_t *)payload;
    p.message.payload.len       = len;
    p.dup_flag                  = 0;
    p.retain_flag               = retain ? 1 : 0;
    if (qos == ALP_MQTT_QOS_0) {
        p.message_id = 0;
    } else {
        if (be->next_msg_id == 0) be->next_msg_id = 1; /* msg-id 0 is reserved */
        p.message_id = be->next_msg_id++;
    }

    int err = mqtt_publish(&be->client, &p);
    return errno_to_alp(err);
#else
    (void)st;
    (void)topic;
    (void)payload;
    (void)len;
    (void)qos;
    (void)retain;
    return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_subscribe(alp_mqtt_backend_state_t *st, const char *topic_filter,
                                alp_mqtt_qos_t qos, alp_mqtt_msg_cb_t cb, void *user)
{
#if defined(CONFIG_ALP_SDK_IOT_MQTT)
    struct mqtt_be *be = (struct mqtt_be *)st->be_data;
    if (be == NULL) return ALP_ERR_NOT_READY;

    be->msg_cb   = cb;
    be->msg_user = user;

    struct mqtt_topic topic = {
        .topic.utf8 = (const uint8_t *)topic_filter,
        .topic.size = strlen(topic_filter),
        .qos        = (uint8_t)qos,
    };
    if (be->next_msg_id == 0) be->next_msg_id = 1;
    struct mqtt_subscription_list list = {
        .list       = &topic,
        .list_count = 1,
        .message_id = be->next_msg_id++,
    };
    int err = mqtt_subscribe(&be->client, &list);
    return errno_to_alp(err);
#else
    (void)st;
    (void)topic_filter;
    (void)qos;
    (void)cb;
    (void)user;
    return ALP_ERR_NOSUPPORT;
#endif
}

static alp_status_t z_loop(alp_mqtt_backend_state_t *st, uint32_t timeout_ms)
{
#if defined(CONFIG_ALP_SDK_IOT_MQTT)
    struct mqtt_be *be = (struct mqtt_be *)st->be_data;
    if (be == NULL) return ALP_ERR_NOT_READY;

    struct zsock_pollfd fds[1] = {0};
    fds[0].fd                  = alp_mqtt_get_fd(&be->client);
    fds[0].events              = ZSOCK_POLLIN;
    int rc                     = zsock_poll(fds, 1, (int)timeout_ms);
    if (rc < 0) return errno_to_alp(-errno);
    if (rc > 0) {
        int err = mqtt_input(&be->client);
        if (err != 0) return errno_to_alp(err);
    }
    int err = mqtt_live(&be->client);
    if (err != 0 && err != -EAGAIN) return errno_to_alp(err);
    return ALP_OK;
#else
    (void)st;
    (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
#endif
}

static void z_close(alp_mqtt_backend_state_t *st)
{
#if defined(CONFIG_ALP_SDK_IOT_MQTT)
    struct mqtt_be *be = (struct mqtt_be *)st->be_data;
    if (be == NULL) return;
    if (be->connected) {
        (void)mqtt_disconnect(&be->client, NULL);
        be->connected = false;
    }
    mqtt_be_release(be);
    st->be_data = NULL;
#else
    (void)st;
#endif
}

/* ------------------------------------------------------------------ */
/* Registration                                                        */
/* ------------------------------------------------------------------ */

static const alp_mqtt_ops_t _ops = {
    .open      = z_open,
    .connect   = z_connect,
    .publish   = z_publish,
    .subscribe = z_subscribe,
    .loop      = z_loop,
    .close     = z_close,
};

ALP_BACKEND_REGISTER(mqtt, zephyr_drv, {
    .silicon_ref = "*",
    .vendor      = "zephyr",
    .base_caps   = 0u,
    .priority    = 100,
    .ops         = &_ops,
    .probe       = NULL,
});
