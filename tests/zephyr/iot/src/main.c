/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smoke tests for the <alp/iot.h> wrapper under native_sim.
 *
 * native_sim has no Wi-Fi radio and no MQTT-capable network stack,
 * so this suite verifies the wrapper's "no networking subsystem
 * present" branch.  The contract under that condition is identical
 * to the v0.1 stub: alp_*_open() returns NULL with
 * alp_last_error() == ALP_ERR_NOSUPPORT (or precisely-stamped
 * ALP_ERR_INVAL for malformed inputs).
 *
 * Real Wi-Fi + MQTT round-trips happen in HW-in-loop CI on
 * AEN-Zephyr (the hil-aen runner).
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"
#include "alp/iot.h"

ZTEST_SUITE(alp_iot, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------ */
/* Wi-Fi station                                                       */
/* ------------------------------------------------------------------ */

ZTEST(alp_iot, test_wifi_open_no_radio_returns_null)
{
    /* native_sim build does not enable CONFIG_ALP_SDK_IOT_WIFI -- the
     * wrapper's "no Wi-Fi subsystem" branch should stamp NOSUPPORT
     * before returning NULL, matching the v0.1 contract. */
    alp_wifi_t *w = alp_wifi_open();
    zassert_is_null(w, "alp_wifi_open without WIFI must yield NULL");
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT, "expected NOSUPPORT, got %d",
                  (int)alp_last_error());
}

ZTEST(alp_iot, test_wifi_connect_null_handle_errors)
{
    alp_status_t s =
        alp_wifi_connect(NULL, &(alp_wifi_credentials_t){.ssid = "alp-lab", .psk = NULL}, 1000);
    zassert_equal(s, ALP_ERR_NOT_READY, "got %d", (int)s);
}

ZTEST(alp_iot, test_wifi_disconnect_null_handle_errors)
{
    zassert_equal(alp_wifi_disconnect(NULL), ALP_ERR_NOT_READY);
}

ZTEST(alp_iot, test_wifi_close_null_is_safe)
{
    /* Idempotent close on NULL must not crash -- mirrors every
     * peripheral wrapper's lifecycle contract. */
    alp_wifi_close(NULL);
}

/* ------------------------------------------------------------------ */
/* MQTT client                                                         */
/* ------------------------------------------------------------------ */

ZTEST(alp_iot, test_mqtt_open_no_stack_returns_null)
{
    /* Same NOSUPPORT fall-through as Wi-Fi when the underlying
     * MQTT stack is absent. */
    alp_mqtt_t *m = alp_mqtt_open(&(alp_mqtt_config_t){
        .broker_uri    = "mqtt://broker.local:1883",
        .client_id     = "alp-iot-smoke",
        .keepalive_s   = 30,
        .clean_session = true,
    });
    zassert_is_null(m, "alp_mqtt_open without MQTT_LIB must yield NULL");
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT, "expected NOSUPPORT, got %d",
                  (int)alp_last_error());
}

ZTEST(alp_iot, test_mqtt_open_null_cfg_invalid)
{
    alp_mqtt_t *m = alp_mqtt_open(NULL);
    zassert_is_null(m);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL, "NULL cfg must stamp INVAL, got %d",
                  (int)alp_last_error());
}

ZTEST(alp_iot, test_mqtt_open_missing_uri_invalid)
{
    alp_mqtt_t *m = alp_mqtt_open(&(alp_mqtt_config_t){
        .broker_uri = NULL,
        .client_id  = "x",
    });
    zassert_is_null(m);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_iot, test_mqtt_open_missing_client_id_invalid)
{
    alp_mqtt_t *m = alp_mqtt_open(&(alp_mqtt_config_t){
        .broker_uri = "mqtt://broker.local:1883",
        .client_id  = NULL,
    });
    zassert_is_null(m);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_iot, test_mqtt_connect_null_handle_errors)
{
    zassert_equal(alp_mqtt_connect(NULL, 1000), ALP_ERR_NOT_READY);
}

ZTEST(alp_iot, test_mqtt_publish_null_handle_errors)
{
    zassert_equal(alp_mqtt_publish(NULL, "topic", (uint8_t[]){0xaa}, 1, ALP_MQTT_QOS_0, false),
                  ALP_ERR_NOT_READY);
}

ZTEST(alp_iot, test_mqtt_subscribe_null_handle_errors)
{
    zassert_equal(alp_mqtt_subscribe(NULL, "topic", ALP_MQTT_QOS_0, NULL, NULL), ALP_ERR_NOT_READY);
}

ZTEST(alp_iot, test_mqtt_loop_null_handle_errors)
{
    zassert_equal(alp_mqtt_loop(NULL, 100), ALP_ERR_NOT_READY);
}

ZTEST(alp_iot, test_mqtt_close_null_is_safe)
{
    alp_mqtt_close(NULL);
}
