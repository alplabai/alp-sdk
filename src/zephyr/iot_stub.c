/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v0.1 stub for <alp/iot.h>.  Every entry point returns
 * ALP_ERR_NOSUPPORT; alp_wifi_open() / alp_mqtt_open() return NULL.
 * The real Zephyr-backed implementation lands in v0.2 (Wi-Fi-station
 * + MQTT publish on AEN) and v0.3 (BLE + HTTP + provisioning).
 *
 * Like camera_stub.c, this exists so applications that
 * #include <alp/iot.h> can link cleanly against v0.1 and discover
 * the lack of support at runtime via NULL handles / NOSUPPORT.
 */

#include "alp/iot.h"

/* ------------------------------------------------------------------ */
/* Wi-Fi station                                                       */
/* ------------------------------------------------------------------ */

alp_wifi_t *alp_wifi_open(void) {
    return NULL;
}

alp_status_t alp_wifi_connect(alp_wifi_t *w,
                              const alp_wifi_credentials_t *creds,
                              uint32_t timeout_ms) {
    (void)w; (void)creds; (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_wifi_disconnect(alp_wifi_t *w) {
    (void)w;
    return ALP_ERR_NOSUPPORT;
}

void alp_wifi_close(alp_wifi_t *w) {
    (void)w;
}

/* ------------------------------------------------------------------ */
/* MQTT client                                                         */
/* ------------------------------------------------------------------ */

alp_mqtt_t *alp_mqtt_open(const alp_mqtt_config_t *cfg) {
    (void)cfg;
    return NULL;
}

alp_status_t alp_mqtt_connect(alp_mqtt_t *m, uint32_t timeout_ms) {
    (void)m; (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_mqtt_publish(alp_mqtt_t *m, const char *topic,
                              const uint8_t *payload, size_t len,
                              alp_mqtt_qos_t qos, bool retain) {
    (void)m; (void)topic; (void)payload; (void)len; (void)qos; (void)retain;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_mqtt_subscribe(alp_mqtt_t *m, const char *topic_filter,
                                alp_mqtt_qos_t qos,
                                alp_mqtt_msg_cb_t cb, void *user) {
    (void)m; (void)topic_filter; (void)qos; (void)cb; (void)user;
    return ALP_ERR_NOSUPPORT;
}

alp_status_t alp_mqtt_loop(alp_mqtt_t *m, uint32_t timeout_ms) {
    (void)m; (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
}

void alp_mqtt_close(alp_mqtt_t *m) {
    (void)m;
}
