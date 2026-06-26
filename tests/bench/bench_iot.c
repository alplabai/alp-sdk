/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Microbenchmarks for <alp/iot.h>.  v0.3 covers the rejection /
 * fast-path costs every IoT consumer pays when the Wi-Fi + MQTT
 * subsystems aren't wired (i.e. native_sim / host builds with
 * CONFIG_ALP_SDK_IOT_WIFI=n / CONFIG_ALP_SDK_IOT_MQTT=n).  v1.0
 * adds real-stack benches (TLS handshake cost, publish latency
 * per QoS, subscribe-then-loop steady-state) once the HW-in-loop
 * runners come online.
 */

#include "bench.h"

#include "alp/iot.h"

void bench_iot_main(void)
{
	/* alp_wifi_open() takes no args -- on the stub path it just
     * returns NULL.  Measures the cost of the wrapper layer when
     * Wi-Fi is unsupported (a typical "probe the radio at boot,
     * fall back to wired" pattern). */
	BENCH_RUN("alp_wifi_open", 1000000, { (void)alp_wifi_open(); });

	/* MQTT NULL-cfg rejection -- earliest exit. */
	BENCH_RUN("alp_mqtt_open(NULL)", 1000000, { (void)alp_mqtt_open(NULL); });

	/* MQTT with a partially-filled cfg -- exercises the
     * stub-backend path's "validate then NOSUPPORT" arm.  The
     * cfg fields are read but never dereferenced on the stub
     * path, so this measures the wrapper's struct-touch cost. */
	BENCH_RUN("alp_mqtt_open(empty cfg)", 1000000, {
		alp_mqtt_config_t cfg = { 0 };
		(void)alp_mqtt_open(&cfg);
	});
}
