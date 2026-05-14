/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file iot.h
 * @brief ALP SDK IoT abstraction (Wi-Fi-station + MQTT in v0.1).
 *
 * Backends:
 *   - Zephyr   : wraps net_*, MQTT client API.
 *   - Yocto    : wraps the Linux network stack + Mosquitto/Paho.
 *   - Baremetal: ALP_ERR_NOSUPPORT (no networking on E1M-AEN bare-metal v0.1).
 *

 * @par ABI status: [ABI-STABLE]
 *      v0.2-v0.4; Wi-Fi station + MQTT (TLS) signatures stable.
 *      See docs/abi-markers.md for the convention.
 */

#ifndef ALP_IOT_H
#define ALP_IOT_H

#include <stdint.h>
#include <stddef.h>
#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* Wi-Fi station                                                       */
/* ------------------------------------------------------------------ */

typedef struct alp_wifi alp_wifi_t;

typedef struct {
    const char *ssid;
    const char *psk;        /**< NULL for open networks. */
} alp_wifi_credentials_t;

/** @brief Acquire the Wi-Fi station singleton handle. */
alp_wifi_t   *alp_wifi_open(void);
/** @brief Associate + authenticate against the AP described by @p creds. */
alp_status_t  alp_wifi_connect(alp_wifi_t *w,
                               const alp_wifi_credentials_t *creds,
                               uint32_t timeout_ms);
/** @brief Drop the current association.  Safe to call when already disconnected. */
alp_status_t  alp_wifi_disconnect(alp_wifi_t *w);
/** @brief Release the Wi-Fi handle.  Idempotent on NULL. */
void          alp_wifi_close(alp_wifi_t *w);

/* ------------------------------------------------------------------ */
/* MQTT client                                                         */
/* ------------------------------------------------------------------ */

typedef struct alp_mqtt alp_mqtt_t;

/**
 * @brief TLS parameters for an `mqtts://` broker connection.
 *
 * All fields are optional.  A NULL @p tls pointer on
 * `alp_mqtt_config_t` is equivalent to `tls = &(alp_mqtt_tls_config_t){0}` --
 * the backend uses the host OS's default CA path
 * (`/etc/ssl/certs` on Debian/Ubuntu/Yocto images) and no client
 * certificate.  Production deployments should pin @p ca_file to a
 * known-good CA bundle.
 */
typedef struct {
    const char *ca_file;        /**< PEM CA bundle path.  NULL = system store. */
    const char *cert_file;      /**< Optional client certificate (PEM). */
    const char *key_file;       /**< Optional client private key (PEM). */
    bool        insecure;       /**< true skips peer cert verification (dev only). */
} alp_mqtt_tls_config_t;

typedef struct {
    const char *broker_uri;     /**< e.g. `"mqtt://broker.local:1883"` or `"mqtts://broker.local:8883"` */
    const char *client_id;
    const char *username;       /**< NULL if unauth */
    const char *password;
    uint16_t    keepalive_s;
    bool        clean_session;
    /**
     * TLS parameters.  Required scheme is `mqtts://`; ignored for
     * `mqtt://`.  NULL is equivalent to a zero-initialised struct
     * (use OS default CA path, no client cert, verify peer).
     */
    const alp_mqtt_tls_config_t *tls;
} alp_mqtt_config_t;

typedef enum {
    ALP_MQTT_QOS_0 = 0,
    ALP_MQTT_QOS_1 = 1,
    ALP_MQTT_QOS_2 = 2
} alp_mqtt_qos_t;

typedef void (*alp_mqtt_msg_cb_t)(const char *topic,
                                  const uint8_t *payload, size_t len,
                                  void *user);

/** @brief Allocate an MQTT client against the broker URI in @p cfg. */
alp_mqtt_t   *alp_mqtt_open(const alp_mqtt_config_t *cfg);
/** @brief Establish the TCP / TLS connection to the broker. */
alp_status_t  alp_mqtt_connect(alp_mqtt_t *m, uint32_t timeout_ms);
/** @brief Publish a single MQTT message. */
alp_status_t  alp_mqtt_publish(alp_mqtt_t *m, const char *topic,
                               const uint8_t *payload, size_t len,
                               alp_mqtt_qos_t qos, bool retain);
/** @brief Subscribe to @p topic_filter and register a per-message callback. */
alp_status_t  alp_mqtt_subscribe(alp_mqtt_t *m, const char *topic_filter,
                                 alp_mqtt_qos_t qos,
                                 alp_mqtt_msg_cb_t cb, void *user);
/** @brief Drive the MQTT state machine for up to @p timeout_ms. */
alp_status_t  alp_mqtt_loop(alp_mqtt_t *m, uint32_t timeout_ms);
/** @brief Disconnect + release the MQTT client.  Idempotent on NULL. */
void          alp_mqtt_close(alp_mqtt_t *m);

#ifdef __cplusplus
}
#endif

#endif  /* ALP_IOT_H */
