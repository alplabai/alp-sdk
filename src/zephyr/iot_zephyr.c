/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/iot.h> -- MQTT publish.
 *
 * Replaces the v0.1 NOSUPPORT stub.  The body is split:
 *
 *   1. The core wrapper (handle pool, last-error, public-API glue)
 *      compiles unconditionally.  Same shape as every other
 *      peripheral wrapper in src/zephyr/.
 *
 *   2. The Zephyr-net glue (mqtt_client calls) is gated on
 *      CONFIG_ALP_SDK_IOT_MQTT.  When OFF (host build, native_sim,
 *      any target without a TCP stack), `alp_mqtt_open()` honours
 *      the v0.1 contract and returns NULL with `alp_last_error()`
 *      = ALP_ERR_NOSUPPORT.
 *
 * The Wi-Fi station half of <alp/iot.h> migrated to the backend
 * registry in Slice 4b -- see src/wifi_dispatch.c +
 * src/backends/wifi/{zephyr_drv,sw_fallback}.c for the public
 * surface.  This file ships only the MQTT body now.
 *
 * The wrapper deliberately stays thin.  Provisioning helpers,
 * MQTT-over-TLS certificate-store binding, and DTLS arrive in v0.3
 * (`<alp/security.h>`).  v0.2 is "publish QoS-0/1 payloads, pump
 * the loop".
 */

#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include "alp/iot.h"
#include "handles.h"

#if defined(CONFIG_ALP_SDK_IOT_MQTT)
#include <zephyr/net/socket.h>
#include <zephyr/net/mqtt.h>
#endif

/* ------------------------------------------------------------------ */
/* Pool sizes                                                          */
/* ------------------------------------------------------------------ */

#ifndef CONFIG_ALP_SDK_MAX_MQTT_HANDLES
#define CONFIG_ALP_SDK_MAX_MQTT_HANDLES 2
#endif

/* MQTT scratch buffers per client.  256 B is enough for the kind of
 * JSON payloads the v0.2 reference apps publish (status + a handful
 * of inference results); apps that publish larger blobs override
 * via CONFIG_ALP_SDK_MQTT_BUF_SIZE. */
#ifndef CONFIG_ALP_SDK_MQTT_BUF_SIZE
#define CONFIG_ALP_SDK_MQTT_BUF_SIZE 256
#endif

/* ------------------------------------------------------------------ */
/* Internal handle structures                                          */
/* ------------------------------------------------------------------ */

struct alp_mqtt {
    bool              in_use;
    alp_mqtt_msg_cb_t msg_cb;
    void             *msg_user;
#if defined(CONFIG_ALP_SDK_IOT_MQTT)
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
#endif
};

#if defined(CONFIG_ALP_SDK_IOT_MQTT)
static struct alp_mqtt  g_mqtt_pool[CONFIG_ALP_SDK_MAX_MQTT_HANDLES];

static struct alp_mqtt *mqtt_pool_acquire(void)
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

static void mqtt_pool_release(struct alp_mqtt *h)
{
    if (h != NULL) h->in_use = false;
}
#endif /* CONFIG_ALP_SDK_IOT_MQTT */

#if defined(CONFIG_ALP_SDK_IOT_MQTT)
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
#endif

/* ================================================================== */
/* MQTT client                                                         */
/* ================================================================== */

#if defined(CONFIG_ALP_SDK_IOT_MQTT)

/* Parse "mqtt(s)?://host[:port]" into host/port/tls.  Returns 0 on
 * success.  No URI-encoding handling — broker addresses in v0.2 are
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
    /* Prefer numeric IPv4 first — keeps the wrapper resolver-free for
     * the common "broker is a static IP" case.  When CONFIG_DNS_RESOLVER
     * is enabled the caller can pre-resolve via getaddrinfo and pass
     * the numeric form. */
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
    struct alp_mqtt *m = CONTAINER_OF(client, struct alp_mqtt, client);

    switch (evt->type) {
    case MQTT_EVT_CONNACK:
        m->connected = (evt->result == 0);
        break;
    case MQTT_EVT_DISCONNECT:
        m->connected = false;
        break;
    case MQTT_EVT_PUBLISH: {
        const struct mqtt_publish_param *pub = &evt->param.publish;
        if (m->msg_cb == NULL) {
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
        size_t topic_len = MIN(pub->message.topic.topic.size, sizeof(m->topic_buf) - 1);
        memcpy(m->topic_buf, pub->message.topic.topic.utf8, topic_len);
        m->topic_buf[topic_len] = '\0';

        /* Read payload directly into rx_buf -- bounded by buffer size. */
        size_t want = MIN(pub->message.payload.len, sizeof(m->rx_buf));
        size_t got  = 0;
        while (got < want) {
            int n = mqtt_read_publish_payload(client, m->rx_buf + got, want - got);
            if (n <= 0) break;
            got += (size_t)n;
        }

        if (pub->message.topic.qos == MQTT_QOS_1_AT_LEAST_ONCE) {
            const struct mqtt_puback_param ack = {.message_id = pub->message_id};
            (void)mqtt_publish_qos1_ack(client, &ack);
        }
        m->msg_cb((const char *)m->topic_buf, m->rx_buf, got, m->msg_user);
        break;
    }
    default:
        break;
    }
}

/* Pull the active socket fd out of the mqtt client.  Path differs
 * between non-secure (transport.tcp) and TLS (transport.tls)
 * variants; v0.2 only ships non-secure (TLS lands with security.h
 * in v0.3) but the helper is shaped to extend cleanly. */
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

alp_mqtt_t *alp_mqtt_open(const alp_mqtt_config_t *cfg)
{
    alp_z_clear_last_error();

    if (cfg == NULL || cfg->broker_uri == NULL || cfg->client_id == NULL) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }

#if defined(CONFIG_ALP_SDK_IOT_MQTT)
    struct alp_mqtt *h = mqtt_pool_acquire();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }

    char     host[64];
    uint16_t port;
    bool     tls;
    int      err = parse_broker_uri(cfg->broker_uri, host, sizeof(host), &port, &tls);
    if (err != 0) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        mqtt_pool_release(h);
        return NULL;
    }

    err = resolve_broker_addr(host, port, &h->broker_addr);
    if (err != 0) {
        alp_z_set_last_error(errno_to_alp(err));
        mqtt_pool_release(h);
        return NULL;
    }

    /* Stash the client id locally so we own its memory across
     * reconnects -- the caller's pointer can go out of scope. */
    strncpy(h->client_id_buf, cfg->client_id, sizeof(h->client_id_buf) - 1);

    mqtt_client_init(&h->client);
    h->client.broker         = &h->broker_addr;
    h->client.evt_cb         = alp_mqtt_evt_cb;
    h->client.client_id.utf8 = (uint8_t *)h->client_id_buf;
    h->client.client_id.size = strlen(h->client_id_buf);
    if (cfg->username != NULL) {
        strncpy(h->username_buf, cfg->username, sizeof(h->username_buf) - 1);
        h->username_utf8.utf8 = (uint8_t *)h->username_buf;
        h->username_utf8.size = strlen(h->username_buf);
        h->client.user_name   = &h->username_utf8;
    } else {
        h->client.user_name = NULL;
    }
    if (cfg->password != NULL) {
        strncpy(h->password_buf, cfg->password, sizeof(h->password_buf) - 1);
        h->password_utf8.utf8 = (uint8_t *)h->password_buf;
        h->password_utf8.size = strlen(h->password_buf);
        h->client.password    = &h->password_utf8;
    } else {
        h->client.password = NULL;
    }
    h->client.protocol_version = MQTT_VERSION_3_1_1;
#if defined(CONFIG_MQTT_LIB_TLS)
    h->client.transport.type   = tls ? MQTT_TRANSPORT_SECURE : MQTT_TRANSPORT_NON_SECURE;
#else
    /* TLS Kconfig disabled at build time — silently downgrade. */
    (void)tls;
    h->client.transport.type   = MQTT_TRANSPORT_NON_SECURE;
#endif
    h->client.rx_buf           = h->rx_buf;
    h->client.rx_buf_size      = sizeof(h->rx_buf);
    h->client.tx_buf           = h->tx_buf;
    h->client.tx_buf_size      = sizeof(h->tx_buf);
    h->client.keepalive        = cfg->keepalive_s;
    h->client.clean_session    = cfg->clean_session ? 1U : 0U;
    h->connected               = false;
    h->next_msg_id             = 1;
    return h;
#else
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
#endif
}

alp_status_t alp_mqtt_connect(alp_mqtt_t *m, uint32_t timeout_ms)
{
    if (m == NULL || !m->in_use) return ALP_ERR_NOT_READY;

#if defined(CONFIG_ALP_SDK_IOT_MQTT)
    int err = mqtt_connect(&m->client);
    if (err != 0) {
        return errno_to_alp(err);
    }

    /* Pump input until we get CONNACK (which the evt cb sets) or the
     * timeout expires.  poll() with a short slice keeps the wait
     * responsive without busy-spinning. */
    uint32_t deadline = k_uptime_get_32() + timeout_ms;
    while ((int32_t)(deadline - k_uptime_get_32()) > 0) {
        struct zsock_pollfd fds[1] = {0};
        fds[0].fd                  = alp_mqtt_get_fd(&m->client);
        fds[0].events              = ZSOCK_POLLIN;
        int rc                     = zsock_poll(fds, 1, 200);
        if (rc < 0) return errno_to_alp(-errno);
        if (rc > 0) {
            err = mqtt_input(&m->client);
            if (err != 0) return errno_to_alp(err);
        }
        err = mqtt_live(&m->client);
        if (err != 0 && err != -EAGAIN) return errno_to_alp(err);

        if (m->connected) return ALP_OK;
    }
    return ALP_ERR_TIMEOUT;
#else
    (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_mqtt_publish(alp_mqtt_t *m, const char *topic, const uint8_t *payload, size_t len,
                              alp_mqtt_qos_t qos, bool retain)
{
    if (m == NULL || !m->in_use) return ALP_ERR_NOT_READY;
    if (topic == NULL) return ALP_ERR_INVAL;
    if (payload == NULL && len > 0) return ALP_ERR_INVAL;

#if defined(CONFIG_ALP_SDK_IOT_MQTT)
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
        if (m->next_msg_id == 0) m->next_msg_id = 1; /* msg-id 0 is reserved */
        p.message_id = m->next_msg_id++;
    }

    int err = mqtt_publish(&m->client, &p);
    return errno_to_alp(err);
#else
    (void)qos;
    (void)retain;
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_mqtt_subscribe(alp_mqtt_t *m, const char *topic_filter, alp_mqtt_qos_t qos,
                                alp_mqtt_msg_cb_t cb, void *user)
{
    if (m == NULL || !m->in_use) return ALP_ERR_NOT_READY;
    if (topic_filter == NULL || cb == NULL) return ALP_ERR_INVAL;

    m->msg_cb   = cb;
    m->msg_user = user;

#if defined(CONFIG_ALP_SDK_IOT_MQTT)
    struct mqtt_topic topic = {
        .topic.utf8 = (const uint8_t *)topic_filter,
        .topic.size = strlen(topic_filter),
        .qos        = (uint8_t)qos,
    };
    if (m->next_msg_id == 0) m->next_msg_id = 1;
    struct mqtt_subscription_list list = {
        .list       = &topic,
        .list_count = 1,
        .message_id = m->next_msg_id++,
    };
    int err = mqtt_subscribe(&m->client, &list);
    return errno_to_alp(err);
#else
    (void)qos;
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_mqtt_loop(alp_mqtt_t *m, uint32_t timeout_ms)
{
    if (m == NULL || !m->in_use) return ALP_ERR_NOT_READY;

#if defined(CONFIG_ALP_SDK_IOT_MQTT)
    struct zsock_pollfd fds[1] = {0};
    fds[0].fd                  = alp_mqtt_get_fd(&m->client);
    fds[0].events              = ZSOCK_POLLIN;
    int rc                     = zsock_poll(fds, 1, (int)timeout_ms);
    if (rc < 0) return errno_to_alp(-errno);
    if (rc > 0) {
        int err = mqtt_input(&m->client);
        if (err != 0) return errno_to_alp(err);
    }
    int err = mqtt_live(&m->client);
    if (err != 0 && err != -EAGAIN) return errno_to_alp(err);
    return ALP_OK;
#else
    (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
#endif
}

void alp_mqtt_close(alp_mqtt_t *m)
{
    if (m == NULL || !m->in_use) return;
#if defined(CONFIG_ALP_SDK_IOT_MQTT)
    if (m->connected) {
        (void)mqtt_disconnect(&m->client, NULL);
        m->connected = false;
    }
    mqtt_pool_release(m);
#endif
}
