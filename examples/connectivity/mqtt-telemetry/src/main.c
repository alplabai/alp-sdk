/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * mqtt-telemetry -- publish a sensor reading over MQTT/TLS.
 *
 * The "connected device" starting point.  It shows the whole
 * telemetry path a field device runs, all on the portable
 * <alp/iot.h> surface:
 *
 *   1. Bring the Wi-Fi station up + associate.   (alp_wifi_*)
 *   2. Open an mqtts:// (TLS) MQTT client.        (alp_mqtt_open)
 *   3. Connect to the broker.                     (alp_mqtt_connect)
 *   4. Publish a telemetry reading on a cadence.  (alp_mqtt_publish)
 *   5. Disconnect cleanly.                        (alp_mqtt_close)
 *
 * Transport (E1M-AEN801): the CC3501E Wi-Fi6+BLE coprocessor bridge
 * (docs/cc3501e-bridge.md), selected automatically by the AEN board
 * emit from `iot.wifi: true` -- the app never names it, it just
 * calls <alp/iot.h>.  The bridge is silicon-validated (2026-06-24);
 * this is a real transport, not a stub.
 *
 * The "sensor reading": to keep the focus on the transport, this
 * template publishes a synthetic metric (device uptime).  Swap
 * read_telemetry_value() for a real sensor read -- e.g. compose it
 * with the `sensor` template (examples/peripheral-io/i2c-master,
 * TMP112 over <alp/chips/tmp112.h>) -- and the publish path here is
 * unchanged.
 *
 * TLS status (preview): the mqtts:// path is configured through the
 * portable API (broker URI + pinned CA), but CONFIG_MBEDTLS is held
 * OFF in the build today -- Zephyr v4.4's mbedtls 3.6 has an
 * ssl_misc.h include-order bug; full tf-psa-crypto wiring is a v0.6
 * work item (see prj.conf / native_sim.conf).  Real AEN silicon
 * uses the TF-M stack.  This is why the catalog record is `preview`.
 *
 * What runs under native_sim (CI lane): there is no radio and no
 * MQTT-capable net stack, so alp_wifi_open()/alp_mqtt_open() return
 * NULL -- ALP_ERR_NOT_READY when a backend registered but its device
 * isn't up, or ALP_ERR_NOSUPPORT when no backend is linked at all;
 * both are the documented <alp/iot.h> NULL contract.  The app
 * detects that, prints the telemetry payloads it WOULD publish (so
 * the framing is CI-observable), and exits on the `[mqtt] done`
 * marker.
 *
 * What runs on AEN HiL: real association through the CC3501E, real
 * MQTT publish to the broker; the same source, no #ifdef in the
 * telemetry logic.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "alp/iot.h"

/* Broker + topic identity.  In a real fleet these come from the
 * device's provisioning step; hardcoded here as documentation.
 * mqtts:// selects the TLS path (see the TLS config below). */
#define MQTT_BROKER_URI "mqtts://broker.example.alplab.ai:8883"
#define MQTT_CLIENT_ID  "e1m-aen801-demo"
#define MQTT_TOPIC      "alp/telemetry/e1m-aen801"

/* Pin the broker's CA bundle.  NULL would fall back to the OS
 * default trust store; a real deployment should pin a known-good
 * CA, so we name the path a customer would provision. */
#define MQTT_CA_FILE "/etc/ssl/certs/alp-broker-ca.pem"

/* Wi-Fi credentials.  On real silicon these come from the EEPROM
 * factory manifest or a provisioning blob so one binary serves a
 * whole fleet; hardcoded for the demo. */
#define WIFI_SSID "alp-demo-ap"
#define WIFI_PSK  "changeme-in-provisioning"

/* Publish a bounded number of readings so the native_sim run
 * terminates and the twister harness latches.  Real firmware would
 * publish forever on a timer. */
#define PUBLISH_COUNT    3u
#define PUBLISH_PERIOD_S 5u

/* Handshake budgets. */
#define WIFI_CONNECT_TIMEOUT_MS 10000u
#define MQTT_CONNECT_TIMEOUT_MS 10000u

/* Read one telemetry value.
 *
 * PLACEHOLDER: returns a synthetic uptime-seconds counter so the
 * template stays chip-free and focused on the transport.  On real
 * hardware, replace the body with a sensor read -- e.g. a TMP112
 * temperature via <alp/chips/tmp112.h> (see the `sensor` template)
 * -- and return that value.  The publish path below does not change. */
static uint32_t read_telemetry_value(uint32_t sample_index)
{
	/* Synthetic: seconds of "uptime" at this sample's cadence.
	 * Deterministic so the native_sim framing output is stable. */
	return sample_index * PUBLISH_PERIOD_S;
}

/* Format a telemetry sample as a compact JSON payload.  Returns the
 * byte length written (excluding the NUL), or 0 on truncation. */
static size_t build_payload(char *buf, size_t cap, uint32_t value)
{
	int n = snprintf(buf,
	                 cap,
	                 "{\"device\":\"%s\",\"metric\":\"uptime_s\",\"value\":%u}",
	                 MQTT_CLIENT_ID,
	                 value);
	if (n < 0 || (size_t)n >= cap) {
		return 0u; /* truncated -- caller skips the publish */
	}
	return (size_t)n;
}

/* Bring the Wi-Fi station up + associate.  Returns an open handle
 * on success, or NULL (with the reason logged) when there is no
 * Wi-Fi backend on this build -- the native_sim / sw_fallback path. */
static alp_wifi_t *telemetry_wifi_up(void)
{
	printf("[mqtt] wifi: opening station\n");
	alp_wifi_t *w = alp_wifi_open();
	if (w == NULL) {
		/* NOT_READY on native_sim (a Wi-Fi backend registered but no
		 * device is up) or NOSUPPORT when no Wi-Fi backend is linked;
		 * on HiL, NOT_READY before the CC3501E bridge has probed. */
		printf("[mqtt] wifi: alp_wifi_open -> NULL (%s)\n", alp_status_name(alp_last_error()));
		return NULL;
	}
	const alp_wifi_credentials_t creds = {
		.ssid = WIFI_SSID,
		.psk  = WIFI_PSK,
	};
	alp_status_t s = alp_wifi_connect(w, &creds, WIFI_CONNECT_TIMEOUT_MS);
	if (s != ALP_OK) {
		printf("[mqtt] wifi: connect -> %s\n", alp_status_name(s));
		alp_wifi_close(w);
		return NULL;
	}
	printf("[mqtt] wifi: associated to \"%s\"\n", WIFI_SSID);
	return w;
}

/* Open + connect the TLS MQTT client.  Returns an open handle, or
 * NULL when there is no MQTT backend on this build. */
static alp_mqtt_t *telemetry_mqtt_up(void)
{
	printf("[mqtt] broker: opening %s (TLS, CA pinned)\n", MQTT_BROKER_URI);

	/* Pin the broker CA for the mqtts:// handshake.  insecure
	 * stays false -- a starter must not teach cert-skipping. */
	static const alp_mqtt_tls_config_t tls = {
		.ca_file  = MQTT_CA_FILE,
		.insecure = false,
	};
	alp_mqtt_config_t cfg = ALP_MQTT_CONFIG_DEFAULT(MQTT_BROKER_URI);
	cfg.client_id         = MQTT_CLIENT_ID;
	cfg.tls               = &tls;

	alp_mqtt_t *m = alp_mqtt_open(&cfg);
	if (m == NULL) {
		printf("[mqtt] broker: alp_mqtt_open -> NULL (%s)\n", alp_status_name(alp_last_error()));
		return NULL;
	}
	alp_status_t s = alp_mqtt_connect(m, MQTT_CONNECT_TIMEOUT_MS);
	if (s != ALP_OK) {
		printf("[mqtt] broker: connect -> %s\n", alp_status_name(s));
		alp_mqtt_close(m);
		return NULL;
	}
	printf("[mqtt] broker: connected\n");
	return m;
}

/* Publish PUBLISH_COUNT readings on the live client. */
static void telemetry_publish_loop(alp_mqtt_t *m)
{
	for (uint32_t i = 0; i < PUBLISH_COUNT; i++) {
		char   payload[96];
		size_t len = build_payload(payload, sizeof(payload), read_telemetry_value(i));
		if (len == 0u) {
			printf("[mqtt] publish %u: payload truncated -- skipped\n", i);
			continue;
		}
		alp_status_t s =
		    alp_mqtt_publish(m, MQTT_TOPIC, (const uint8_t *)payload, len, ALP_MQTT_QOS_1, false);
		printf("[mqtt] publish %u -> %s: %s\n", i, alp_status_name(s), payload);

		/* Pace the publishes -- a real telemetry device sends on a
		 * timer, not in a tight loop.  alp_delay_ms is portable
		 * (<alp/peripheral.h>, reached via <alp/iot.h>). */
		if (i + 1u < PUBLISH_COUNT) {
			alp_delay_ms(PUBLISH_PERIOD_S * 1000u);
		}
	}
}

/* native_sim / no-backend path: there's no live client, but print
 * the payloads the app WOULD publish so the framing is observable
 * in CI (and so a customer sees the message shape without a broker). */
static void telemetry_print_framing(void)
{
	printf("[mqtt] no transport on this build -- printing framing only\n");
	for (uint32_t i = 0; i < PUBLISH_COUNT; i++) {
		char   payload[96];
		size_t len = build_payload(payload, sizeof(payload), read_telemetry_value(i));
		if (len == 0u) {
			continue;
		}
		printf("[mqtt] would publish to %s: %s\n", MQTT_TOPIC, payload);
	}
}

int main(void)
{
	/* Bring up the SDK runtime before anything else. */
	(void)alp_init();

	printf("[mqtt] alp-sdk mqtt-telemetry demo (publish over MQTT/TLS)\n");
	printf("[mqtt] transport: CC3501E Wi-Fi bridge (E1M-AEN801)\n");

	alp_wifi_t *w = telemetry_wifi_up();
	if (w != NULL) {
		alp_mqtt_t *m = telemetry_mqtt_up();
		if (m != NULL) {
			telemetry_publish_loop(m);
			alp_mqtt_close(m);
		}
		alp_wifi_disconnect(w);
		alp_wifi_close(w);
	} else {
		/* No Wi-Fi backend (native_sim): show the framing so the
		 * run is still meaningful + the harness has a marker. */
		telemetry_print_framing();
	}

	printf("[mqtt] done\n");
	return 0;
}
