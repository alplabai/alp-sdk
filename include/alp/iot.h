/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file iot.h
 * @brief Alp SDK IoT abstraction (Wi-Fi-station + MQTT in v0.1).
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
#include "alp/cap_instance.h"
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
	const char *psk; /**< NULL for open networks. */
} alp_wifi_credentials_t;

/**
 * @brief Acquire the Wi-Fi station singleton handle.
 *
 * @return Open handle on success; NULL with @ref alp_last_error set
 *         to @ref ALP_ERR_NOSUPPORT (no Wi-Fi backend wired) or
 *         @ref ALP_ERR_NOT_READY (driver not yet probed).
 */
alp_wifi_t *alp_wifi_open(void);

/**
 * @brief Associate + authenticate against the AP described by @p creds.
 *
 * @param[in] w           Handle from @ref alp_wifi_open.
 * @param[in] creds       SSID + PSK.  PSK may be NULL for open networks.
 * @param[in] timeout_ms  Max wait for the four-way handshake.
 *
 * @return ALP_OK / ALP_ERR_INVAL (NULL handle / creds / SSID) /
 *         ALP_ERR_TIMEOUT / ALP_ERR_IO (auth failed) /
 *         ALP_ERR_NOSUPPORT.
 */
alp_status_t
alp_wifi_connect(alp_wifi_t *w, const alp_wifi_credentials_t *creds, uint32_t timeout_ms);

/**
 * @brief Drop the current association.  Safe to call when already disconnected.
 *
 * @param[in] w  Handle from @ref alp_wifi_open.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_wifi_disconnect(alp_wifi_t *w);

/**
 * @brief Release the Wi-Fi handle.  Idempotent on NULL.
 *
 * @param[in] w  Handle from @ref alp_wifi_open, or NULL.
 */
void alp_wifi_close(alp_wifi_t *w);

/**
 * @brief Query the capabilities of an opened Wi-Fi handle.
 *
 * @param w  Handle from @ref alp_wifi_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p w is NULL.
 */
const alp_capabilities_t *alp_wifi_capabilities(const alp_wifi_t *w);

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
	const char *ca_file;   /**< PEM CA bundle path.  NULL = system store. */
	const char *cert_file; /**< Optional client certificate (PEM). */
	const char *key_file;  /**< Optional client private key (PEM). */
	bool        insecure;  /**< true skips peer cert verification (dev only). */
} alp_mqtt_tls_config_t;

typedef struct {
	const char
	    *broker_uri; /**< e.g. `"mqtt://broker.local:1883"` or `"mqtts://broker.local:8883"` */
	const char *client_id;
	const char *username; /**< NULL if unauth */
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

typedef enum { ALP_MQTT_QOS_0 = 0, ALP_MQTT_QOS_1 = 1, ALP_MQTT_QOS_2 = 2 } alp_mqtt_qos_t;

typedef void (*alp_mqtt_msg_cb_t)(const char    *topic,
                                  const uint8_t *payload,
                                  size_t         len,
                                  void          *user);

/**
 * @brief Allocate an MQTT client against the broker URI in @p cfg.
 *
 * Does not open a network connection -- call @ref alp_mqtt_connect
 * for that.  The split lets callers configure listener callbacks
 * via @ref alp_mqtt_subscribe before the broker handshake.
 *
 * @param[in] cfg  Broker URI + client id + optional TLS config.
 *                 Must be non-NULL.
 *
 * @return Open client on success; NULL with @ref alp_last_error
 *         set to @ref ALP_ERR_INVAL / @ref ALP_ERR_NOSUPPORT
 *         (no MQTT backend wired).
 */
alp_mqtt_t *alp_mqtt_open(const alp_mqtt_config_t *cfg);

/**
 * @brief Establish the TCP / TLS connection to the broker.
 *
 * @param[in] m           Handle from @ref alp_mqtt_open.
 * @param[in] timeout_ms  Max wait for the CONNECT handshake.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_TIMEOUT /
 *         ALP_ERR_IO (TLS or CONNECT error) / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_mqtt_connect(alp_mqtt_t *m, uint32_t timeout_ms);

/**
 * @brief Publish a single MQTT message.
 *
 * @param[in] m        Handle from @ref alp_mqtt_open.
 * @param[in] topic    Topic name; must be non-NULL.
 * @param[in] payload  Message body; may be NULL when @p len == 0.
 * @param[in] len      Body length.
 * @param[in] qos      Quality-of-service level.
 * @param[in] retain   Set the broker's retain flag.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY (not connected) /
 *         ALP_ERR_IO / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_mqtt_publish(alp_mqtt_t    *m,
                              const char    *topic,
                              const uint8_t *payload,
                              size_t         len,
                              alp_mqtt_qos_t qos,
                              bool           retain);

/**
 * @brief Subscribe to @p topic_filter and register a per-message callback.
 *
 * Callback runs from the MQTT loop's context; do minimal work there.
 *
 * @param[in] m             Handle from @ref alp_mqtt_open.
 * @param[in] topic_filter  MQTT topic filter (may contain `+` / `#`).
 * @param[in] qos           Maximum QoS the broker should deliver at.
 * @param[in] cb            Per-message callback.  Must be non-NULL.
 * @param[in] user          Opaque pointer forwarded to @p cb.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY / ALP_ERR_IO /
 *         ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_mqtt_subscribe(
    alp_mqtt_t *m, const char *topic_filter, alp_mqtt_qos_t qos, alp_mqtt_msg_cb_t cb, void *user);

/**
 * @brief Drive the MQTT state machine for up to @p timeout_ms.
 *
 * Caller-driven loop: customers typically run this from a dedicated
 * thread or as part of a main super-loop.  Returns when a callback
 * has fired, a keepalive ping is needed, or @p timeout_ms elapses.
 *
 * @param[in] m           Handle from @ref alp_mqtt_open.
 * @param[in] timeout_ms  Max wait per call.
 *
 * @return ALP_OK / ALP_ERR_INVAL / ALP_ERR_TIMEOUT (no event in
 *         window) / ALP_ERR_IO / ALP_ERR_NOSUPPORT.
 */
alp_status_t alp_mqtt_loop(alp_mqtt_t *m, uint32_t timeout_ms);

/**
 * @brief Disconnect + release the MQTT client.  Idempotent on NULL.
 *
 * @param[in] m  Handle from @ref alp_mqtt_open, or NULL.
 */
void alp_mqtt_close(alp_mqtt_t *m);

/**
 * @brief Query the capabilities of an opened MQTT handle.
 *
 * @param m  Handle from @ref alp_mqtt_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p m is NULL.
 */
const alp_capabilities_t *alp_mqtt_capabilities(const alp_mqtt_t *m);

#ifdef __cplusplus
}
#endif

#endif /* ALP_IOT_H */
