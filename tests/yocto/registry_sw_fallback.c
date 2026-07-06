/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plain-CMake tests for the Yocto-side MQTT / Security / Wi-Fi / BLE
 * registry wiring at the sw_fallback tier (#33).
 *
 * These four classes migrated off the stub_backend.c direct-impl model
 * onto the dispatcher + backend-registry pattern.  This test compiles
 * ONLY the priority-0 sw_fallback backends into the executable (never
 * the vendor yocto_drv backends), so the dispatcher deterministically
 * selects sw_fallback regardless of which optional host libraries
 * (libmosquitto / OpenSSL) are installed -- it is runnable on every
 * Linux CI host.  The real libmosquitto / OpenSSL paths are covered by
 * tests/yocto/iot_mqtt.c + tests/yocto/security_openssl.c when the
 * libs are present.
 *
 * What this proves:
 *   - the dispatchers link on Yocto and own the public symbols
 *     (previously stub_backend.c did, and alp_wifi_capabilities /
 *     alp_mqtt_capabilities didn't exist at all on Yocto);
 *   - dispatcher-level argument validation semantics (NULL handle ->
 *     NOT_READY, bad args -> INVAL) match the Zephyr side;
 *   - sw_fallback tier contracts (open OK / IO NOT_IMPLEMENTED, or
 *     open NOSUPPORT for security).
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_registry_sw_fallback
 *   ctest --test-dir build -R alp_test_registry_sw_fallback
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/ble.h"
#include "alp/iot.h"
#include "alp/peripheral.h"
#include "alp/security.h"

#include "test_assert.h"

/* ------------------------------------------------------------------ */
/* MQTT (sw_fallback: open OK, I/O ops NOT_IMPLEMENTED)                 */
/* ------------------------------------------------------------------ */

static void test_mqtt_open_null_cfg_stamps_invalid(void)
{
	alp_mqtt_t *m = alp_mqtt_open(NULL);
	ALP_ASSERT_NULL(m);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_mqtt_fallback_open_and_capabilities(void)
{
	alp_mqtt_config_t cfg = {
		.broker_uri    = "mqtt://127.0.0.1:1883",
		.client_id     = "alp-test",
		.keepalive_s   = 30,
		.clean_session = true,
	};
	alp_mqtt_t *m = alp_mqtt_open(&cfg);
	ALP_ASSERT_TRUE(m != NULL);
	if (m != NULL) {
		const alp_capabilities_t *caps = alp_mqtt_capabilities(m);
		ALP_ASSERT_TRUE(caps != NULL);
		if (caps != NULL) {
			ALP_ASSERT_EQ_INT((int)caps->flags, 0);
		}
		/* No real broker under sw_fallback -- I/O ops refuse. */
		ALP_ASSERT_EQ_INT(alp_mqtt_connect(m, 100), ALP_ERR_NOT_IMPLEMENTED);
		const uint8_t payload[] = "x";
		ALP_ASSERT_EQ_INT(alp_mqtt_publish(m, "t", payload, 1, ALP_MQTT_QOS_0, false),
		                  ALP_ERR_NOT_IMPLEMENTED);
		ALP_ASSERT_EQ_INT(alp_mqtt_loop(m, 10), ALP_ERR_NOT_IMPLEMENTED);
		alp_mqtt_close(m);
	}
}

static void test_mqtt_null_handle_ops_not_ready(void)
{
	/* Dispatcher contract (matches the Zephyr side): a NULL handle is
	 * NOT_READY, not INVAL -- INVAL is reserved for bad arguments on a
	 * live handle. */
	const uint8_t payload[] = "x";
	ALP_ASSERT_EQ_INT(alp_mqtt_connect(NULL, 100), ALP_ERR_NOT_READY);
	ALP_ASSERT_EQ_INT(alp_mqtt_publish(NULL, "t", payload, 1, ALP_MQTT_QOS_0, false),
	                  ALP_ERR_NOT_READY);
	ALP_ASSERT_EQ_INT(alp_mqtt_loop(NULL, 10), ALP_ERR_NOT_READY);
	alp_mqtt_close(NULL); /* must be safe */
	ALP_TEST_PASS();
}

/* ------------------------------------------------------------------ */
/* Security (sw_fallback: opens NOSUPPORT, random NOSUPPORT)            */
/* ------------------------------------------------------------------ */

static void test_security_fallback_opens_nosupport(void)
{
	alp_hash_t *h = alp_hash_open(ALP_HASH_SHA256);
	ALP_ASSERT_NULL(h);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_NOSUPPORT);

	uint8_t     key[16] = { 0 };
	alp_aead_t *a       = alp_aead_open(ALP_AEAD_AES_128_GCM, key, sizeof(key));
	ALP_ASSERT_NULL(a);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_NOSUPPORT);
}

static void test_security_dispatcher_validation(void)
{
	/* Out-of-range alg is rejected by the dispatcher BEFORE backend
	 * selection -- INVAL, not NOSUPPORT. */
	alp_hash_t *h = alp_hash_open((alp_hash_alg_t)99);
	ALP_ASSERT_NULL(h);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);

	alp_aead_t *a = alp_aead_open(ALP_AEAD_AES_128_GCM, NULL, 16);
	ALP_ASSERT_NULL(a);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);

	ALP_ASSERT_EQ_INT(alp_hash_update(NULL, (const uint8_t *)"x", 1), ALP_ERR_NOT_READY);
	ALP_ASSERT_EQ_INT(alp_random_bytes(NULL, 16), ALP_ERR_INVAL);
	uint8_t buf[8];
	ALP_ASSERT_EQ_INT(alp_random_bytes(buf, sizeof(buf)), ALP_ERR_NOSUPPORT);
}

/* ------------------------------------------------------------------ */
/* Wi-Fi (sw_fallback: open OK, connect NOT_IMPLEMENTED)                */
/* ------------------------------------------------------------------ */

static void test_wifi_fallback_open_and_capabilities(void)
{
	alp_wifi_t *w = alp_wifi_open();
	ALP_ASSERT_TRUE(w != NULL);
	if (w != NULL) {
		/* alp_wifi_capabilities was declared in <alp/iot.h> but had NO
		 * definition on Yocto before #33 -- calling it here proves the
		 * link gap is closed. */
		const alp_capabilities_t *caps = alp_wifi_capabilities(w);
		ALP_ASSERT_TRUE(caps != NULL);
		alp_wifi_credentials_t creds = { .ssid = "test", .psk = NULL };
		ALP_ASSERT_EQ_INT(alp_wifi_connect(w, &creds, 100), ALP_ERR_NOT_IMPLEMENTED);
		ALP_ASSERT_EQ_INT(alp_wifi_connect(w, NULL, 100), ALP_ERR_INVAL);
		alp_wifi_close(w);
	}
	ALP_ASSERT_EQ_INT(alp_wifi_connect(NULL, NULL, 0), ALP_ERR_NOT_READY);
}

/* ------------------------------------------------------------------ */
/* BLE (sw_fallback: open + advertise OK, GATT NOT_IMPLEMENTED)         */
/* ------------------------------------------------------------------ */

static void test_ble_fallback_open_and_advertise(void)
{
	alp_ble_t *b = alp_ble_open();
	ALP_ASSERT_TRUE(b != NULL);
	if (b != NULL) {
		const alp_capabilities_t *caps = alp_ble_capabilities(b);
		ALP_ASSERT_TRUE(caps != NULL);
		alp_ble_adv_config_t cfg = {
			.name        = "alp-test",
			.connectable = false,
		};
		ALP_ASSERT_EQ_INT(alp_ble_advertise_start(b, &cfg), ALP_OK);
		ALP_ASSERT_EQ_INT(alp_ble_advertise_stop(b), ALP_OK);
		ALP_ASSERT_EQ_INT(alp_ble_advertise_start(b, NULL), ALP_ERR_INVAL);
		alp_ble_close(b);
	}
	ALP_ASSERT_EQ_INT(alp_ble_advertise_stop(NULL), ALP_ERR_NOT_READY);
}

int main(void)
{
	test_mqtt_open_null_cfg_stamps_invalid();
	test_mqtt_fallback_open_and_capabilities();
	test_mqtt_null_handle_ops_not_ready();

	test_security_fallback_opens_nosupport();
	test_security_dispatcher_validation();

	test_wifi_fallback_open_and_capabilities();

	test_ble_fallback_open_and_advertise();

	ALP_TEST_SUMMARY();
}
