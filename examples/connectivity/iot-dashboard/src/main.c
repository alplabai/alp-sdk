/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * iot-dashboard
 * =============
 *
 * Customer-facing IoT demo: an E1M-AEN module reads BME280
 * environment samples (temperature, humidity, pressure), publishes
 * them to an MQTT broker over TLS, and renders the live values +
 * connection state on a 240×320 LVGL dashboard.
 *
 *
 * ── End-to-end SDK demo ────────────────────────────────────────
 *
 *   ┌──────────────────┐  I²C0   ┌─────────┐
 *   │  E1M-AEN701 SoM  │ ──────▶ │ BME280  │ (T / H / P)
 *   │ + Cortex-M55 HP  │         └─────────┘
 *   └────────┬─────────┘
 *            │  SDIO  ┌─────────────────────────┐
 *            ├──────▶ │ Murata LBEE5HY2FY WiFi  │ ── TLS ─▶ broker.example
 *            │        └─────────────────────────┘
 *            │  I²C0  ┌─────────────────────────┐
 *            ├──────▶ │ OPTIGA Trust M          │  handshake offload
 *            │        └─────────────────────────┘
 *            │  SPI1  ┌─────────────────────────┐
 *            └──────▶ │ ST7789 240×320 TFT      │ ── LVGL dashboard
 *                     └─────────────────────────┘
 *
 *
 * ── What "verified" would mean ─────────────────────────────────
 *
 * Today the path is paper-correct; HiL flips it to verified when:
 *   1. WiFi associates with a real AP.
 *   2. TLS handshake completes through OPTIGA in < 1 s.
 *   3. The dashboard refreshes within 250 ms of a BME280 sample.
 *   4. The publish/connect cycle survives a 24h soak.
 * That's the v0.6 AEN HiL story.
 *
 * native_sim builds end-to-end but stubs WiFi/TLS -- runs the
 * UI logic against simulated sensor values.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/display.h>
#include <stdio.h>

#include <lvgl.h>

#include "alp/peripheral.h"
#include "alp/iot.h"
#include "alp/chips/bme280.h"

/* EVK_I2C_BUS_SENSORS is a board-macro from the generated routes header
 * (= E1M_I2C0); rebind it in board.yaml `pins:` to port to another board. */
#include "alp/boards/alp_e1m_evk_routes.h"

#include "dashboard_ui.h"

LOG_MODULE_REGISTER(iot_dashboard, LOG_LEVEL_INF);

static bme280_t    s_env;
static alp_i2c_t  *s_i2c;
static alp_mqtt_t *s_mqtt;

/* The shared dashboard snapshot.  Single-writer (sensor thread)
 * single-reader (main UI thread); volatile-snapshot read in
 * main() is correct without locks. */
static dashboard_state_t g_state;

K_THREAD_STACK_DEFINE(sensor_stack, 4096);
static struct k_thread sensor_thread;

static void sensor_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);
	/* Sample at 1 Hz -- BME280 is slow + customers want a
     * comfortable refresh rate on the dashboard. */
	while (1) {
		bme280_raw_t raw = { 0 };
		if (bme280_read_raw(&s_env, &raw) == ALP_OK) {
			bme280_compensated_t cc = { 0 };
			bme280_compensate(&s_env, &raw, &cc);
			g_state.temp_c         = cc.temperature_c100 / 100.f;
			g_state.humid_pct      = cc.humidity_milli_pct / 1024.f;
			g_state.pressure_hpa   = cc.pressure_pa / 100.f;
			g_state.last_update_ms = k_uptime_get_32();

			/* Push the same sample to MQTT if connected. */
			if (s_mqtt != NULL) {
				char payload[96];
				int  n = snprintf(payload,
				                  sizeof(payload),
				                  "{\"t\":%.2f,\"h\":%.1f,\"p\":%.1f}",
				                  (double)g_state.temp_c,
				                  (double)g_state.humid_pct,
				                  (double)g_state.pressure_hpa);
				if (n > 0) {
					(void)alp_mqtt_publish(s_mqtt,
					                       "alp/env",
					                       (const uint8_t *)payload,
					                       (size_t)n,
					                       ALP_MQTT_QOS_0,
					                       /*retain=*/false);
				}
			}
		}
		k_msleep(1000);
	}
}

int main(void)
{
	LOG_INF("iot-dashboard demo starting");

	/* Sensor bus + chip bring-up. */
	s_i2c = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = EVK_I2C_BUS_SENSORS, /* = E1M_I2C0 */
	    .bitrate_hz = 400000,
	});
	if (s_i2c == NULL || bme280_init(&s_env, s_i2c, BME280_I2C_ADDR_LOW) != ALP_OK) {
		LOG_WRN("BME280 unavailable; dashboard will show zero readings");
	} else {
		bme280_set_sampling(&s_env,
		                    BME280_OVERSAMPLING_X1,
		                    BME280_OVERSAMPLING_X1,
		                    BME280_OVERSAMPLING_X1,
		                    BME280_MODE_NORMAL,
		                    BME280_STANDBY_125_MS,
		                    BME280_FILTER_OFF);
	}

	/* WiFi + MQTT connect.  On native_sim this drops to the
     * <alp/iot.h> NOSUPPORT stub -- the UI still runs against
     * the in-memory dashboard_state_t. */
	alp_wifi_t *wifi = alp_wifi_open();
	if (wifi) {
		(void)alp_wifi_connect(wifi,
		                       &(alp_wifi_credentials_t){
		                           .ssid = "alp-demo-ssid",
		                           .psk  = "demo-password",
		                       },
		                       /*timeout_ms=*/5000);
	}
	static const alp_mqtt_tls_config_t s_tls = { 0 }; /* defaults: OS CA, verify peer. */
	s_mqtt                                   = alp_mqtt_open(&(alp_mqtt_config_t){
	    .broker_uri = "mqtts://broker.example.com:8883",
	    .client_id  = "alp-iot-dashboard-demo",
	    .tls        = &s_tls,
	});
	if (s_mqtt == NULL) {
		LOG_WRN("MQTT broker unreachable; running standalone");
	}

	/* Display + LVGL bring-up. */
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	if (!device_is_ready(display)) {
		LOG_ERR("display %s not ready", display->name);
		return 1;
	}
	lv_init();
	dashboard_ui_build();
	display_blanking_off(display);

	/* Spawn the sensor thread. */
	k_thread_create(&sensor_thread,
	                sensor_stack,
	                K_THREAD_STACK_SIZEOF(sensor_stack),
	                sensor_entry,
	                NULL,
	                NULL,
	                NULL,
	                K_PRIO_PREEMPT(4),
	                0,
	                K_NO_WAIT);
	k_thread_name_set(&sensor_thread, "env_sensor");

	/* Render loop. */
	while (1) {
		dashboard_state_t snap = g_state;
		snap.mqtt_connected    = (s_mqtt != NULL);
		dashboard_ui_apply(&snap);

		const uint32_t sleep_ms = lv_task_handler();
		k_msleep(MIN(sleep_ms, 10u));
	}

	return 0;
}
