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

static void test_mqtts_uri_with_default_tls_opens(void)
{
    /* mqtts:// + tls=NULL falls back to the system CA path; open()
     * should succeed (connect() is what proves the TLS handshake
     * against a real broker, parked behind HIL). */
    alp_mqtt_config_t cfg = {
        .broker_uri    = "mqtts://example.com:8883",
        .client_id     = "alp-test",
        .keepalive_s   = 30,
        .clean_session = true,
        .tls           = NULL,
    };
    alp_mqtt_t *m = alp_mqtt_open(&cfg);
    ALP_ASSERT_TRUE(m != NULL);
    if (m != NULL) {
        alp_mqtt_close(m);
    }
}

static void test_mqtts_uri_default_port_8883(void)
{
    /* No explicit port -- the parser should default to 8883 for
     * mqtts://.  Open succeeds regardless; this just confirms the
     * URI parser accepts the bare host. */
    alp_mqtt_config_t cfg = {
        .broker_uri    = "mqtts://example.com",
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

static void test_mqtts_uri_with_explicit_ca_bundle_opens(void)
{
    /* Mosquitto accepts the CA file path eagerly (loads + parses
     * here).  Use /etc/ssl/certs/ca-certificates.crt which exists
     * on every Debian/Ubuntu/Yocto runner libmosquitto-dev ships
     * on; a malformed path is exercised in the next test. */
    alp_mqtt_tls_config_t tls = {
        .ca_file = "/etc/ssl/certs/ca-certificates.crt",
    };
    alp_mqtt_config_t cfg = {
        .broker_uri    = "mqtts://example.com:8883",
        .client_id     = "alp-test",
        .keepalive_s   = 30,
        .clean_session = true,
        .tls           = &tls,
    };
    alp_mqtt_t *m = alp_mqtt_open(&cfg);
    ALP_ASSERT_TRUE(m != NULL);
    if (m != NULL) {
        alp_mqtt_close(m);
    }
}

static void test_mqtts_uri_with_missing_ca_file_fails(void)
{
    /* Bogus CA path -- mosquitto_tls_set fails immediately; we
     * surface that as ALP_ERR_IO via mosq_to_alp. */
    alp_mqtt_tls_config_t tls = {
        .ca_file = "/nonexistent/path/ca.pem",
    };
    alp_mqtt_config_t cfg = {
        .broker_uri    = "mqtts://example.com:8883",
        .client_id     = "alp-test",
        .keepalive_s   = 30,
        .clean_session = true,
        .tls           = &tls,
    };
    alp_mqtt_t *m = alp_mqtt_open(&cfg);
    ALP_ASSERT_NULL(m);
    /* mosquitto returns MOSQ_ERR_INVAL for a missing CA file in
     * some versions and an error mapped to IO in others; we just
     * assert the open refused. */
    ALP_ASSERT_TRUE(alp_last_error() != ALP_OK);
}

static void test_mqtts_uri_insecure_flag_accepted(void)
{
    /* insecure=true is a dev-only escape hatch; it must not break
     * open() and must not require a CA file. */
    alp_mqtt_tls_config_t tls = {
        .insecure = true,
    };
    alp_mqtt_config_t cfg = {
        .broker_uri    = "mqtts://example.com:8883",
        .client_id     = "alp-test",
        .keepalive_s   = 30,
        .clean_session = true,
        .tls           = &tls,
    };
    alp_mqtt_t *m = alp_mqtt_open(&cfg);
    ALP_ASSERT_TRUE(m != NULL);
    if (m != NULL) {
        alp_mqtt_close(m);
    }
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
    test_mqtts_uri_with_default_tls_opens();
    test_mqtts_uri_default_port_8883();
    test_mqtts_uri_with_explicit_ca_bundle_opens();
    test_mqtts_uri_with_missing_ca_file_fails();
    test_mqtts_uri_insecure_flag_accepted();
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
