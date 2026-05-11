/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plain-CMake tests for the Yocto/libmosquitto MQTT backend
 * (src/yocto/iot_yocto.c).
 *
 * Failure-path coverage only -- broker-roundtrip happy paths need
 * a Mosquitto broker on the runner, which is parked behind
 * ci/HW-IN-LOOP.md alongside the on-device coverage.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_iot_mqtt
 *   ctest --test-dir build -R alp_test_iot_mqtt
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/iot.h"
#include "alp/peripheral.h"

#include "test_assert.h"

static void test_null_cfg_returns_null_and_stamps_invalid(void)
{
    alp_mqtt_t *m = alp_mqtt_open(NULL);
    ALP_ASSERT_NULL(m);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_null_broker_uri_returns_null_and_stamps_invalid(void)
{
    alp_mqtt_config_t cfg = {
        .broker_uri    = NULL,
        .client_id     = "alp-test",
        .keepalive_s   = 30,
        .clean_session = true,
    };
    alp_mqtt_t *m = alp_mqtt_open(&cfg);
    ALP_ASSERT_NULL(m);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_mqtts_uri_returns_nosupport(void)
{
    /* TLS deferred to v0.4 secure-stack work. */
    alp_mqtt_config_t cfg = {
        .broker_uri    = "mqtts://example.com:8883",
        .client_id     = "alp-test",
        .keepalive_s   = 30,
        .clean_session = true,
    };
    alp_mqtt_t *m = alp_mqtt_open(&cfg);
    ALP_ASSERT_NULL(m);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_NOSUPPORT);
}

static void test_unknown_scheme_returns_invalid(void)
{
    alp_mqtt_config_t cfg = {
        .broker_uri    = "http://example.com",
        .client_id     = "alp-test",
        .keepalive_s   = 30,
        .clean_session = true,
    };
    alp_mqtt_t *m = alp_mqtt_open(&cfg);
    ALP_ASSERT_NULL(m);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_empty_host_returns_invalid(void)
{
    alp_mqtt_config_t cfg = {
        .broker_uri    = "mqtt://",
        .client_id     = "alp-test",
        .keepalive_s   = 30,
        .clean_session = true,
    };
    alp_mqtt_t *m = alp_mqtt_open(&cfg);
    ALP_ASSERT_NULL(m);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_bad_port_returns_invalid(void)
{
    alp_mqtt_config_t cfg = {
        .broker_uri    = "mqtt://example.com:99999",
        .client_id     = "alp-test",
        .keepalive_s   = 30,
        .clean_session = true,
    };
    alp_mqtt_t *m = alp_mqtt_open(&cfg);
    ALP_ASSERT_NULL(m);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_valid_uri_opens_successfully(void)
{
    /* Open succeeds even without a reachable broker -- connect()
     * is what actually contacts the broker.  This proves the URI
     * parser + mosquitto_new + callback setup all work. */
    alp_mqtt_config_t cfg = {
        .broker_uri    = "mqtt://127.0.0.1:1883",
        .client_id     = "alp-test",
        .keepalive_s   = 30,
        .clean_session = true,
    };
    alp_mqtt_t *m = alp_mqtt_open(&cfg);
    ALP_ASSERT_TRUE(m != NULL);
    if (m != NULL) {
        alp_mqtt_close(m);
    }
}

static void test_publish_on_null_returns_invalid(void)
{
    const uint8_t payload[] = "hello";
    alp_status_t  rc = alp_mqtt_publish(NULL, "t", payload, sizeof(payload), ALP_MQTT_QOS_0, false);
    ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_subscribe_on_null_returns_invalid(void)
{
    alp_status_t rc =
        alp_mqtt_subscribe(NULL, "t", ALP_MQTT_QOS_0, (alp_mqtt_msg_cb_t)0, (void *)0);
    ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_loop_on_null_returns_invalid(void)
{
    alp_status_t rc = alp_mqtt_loop(NULL, 100);
    ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

static void test_close_null_is_safe(void)
{
    alp_mqtt_close(NULL);
    ALP_TEST_PASS();
}

int main(void)
{
    test_null_cfg_returns_null_and_stamps_invalid();
    test_null_broker_uri_returns_null_and_stamps_invalid();
    test_mqtts_uri_returns_nosupport();
    test_unknown_scheme_returns_invalid();
    test_empty_host_returns_invalid();
    test_bad_port_returns_invalid();
    test_valid_uri_opens_successfully();
    test_publish_on_null_returns_invalid();
    test_subscribe_on_null_returns_invalid();
    test_loop_on_null_returns_invalid();
    test_close_null_is_safe();

    ALP_TEST_SUMMARY();
}
