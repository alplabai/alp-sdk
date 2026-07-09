/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * IoT connected-camera reference application — v0.1 skeleton.
 *
 * Mirrors the EdgeAI vision skeleton's structure but targets the
 * V2N family for v0.3 ("IoT + display polish") rather than AEN.
 * Six stages: peripherals, camera, classifier, network, mqtt, loop.
 *
 * Each stage either runs the real v0.1 SDK call (when the surface
 * is implemented) or prints "skip" and continues so CI can verify
 * the example compiles + runs end-to-end under native_sim.  v0.2
 * and v0.3 implementations slot into the TODO blocks in-place.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/board.h"
#include "alp/chips/ssd1306.h"
#include "alp/blocks/button_led.h"
#include "alp/camera.h"
#include "alp/iot.h"

/* ------------------------------------------------------------------ */
/* Stage 1 — peripherals                                               */
/* ------------------------------------------------------------------ */

static alp_i2c_t       *g_sensor_bus;
static ssd1306_t        g_oled;
static alp_button_led_t g_trigger;

static int stage_peripherals_init(void)
{
	printf("[iotcam] stage 1: peripherals\n");

	g_sensor_bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = BOARD_I2C_SENSORS,
	    .bitrate_hz = 400000,
	});
	if (g_sensor_bus == NULL) {
		printf("[iotcam]   alp_i2c_open                  failed\n");
		return -1;
	}
	printf("[iotcam]   alp_i2c_open(I2C0)            ok\n");

	alp_status_t s = ssd1306_init(&g_oled, g_sensor_bus, SSD1306_I2C_ADDR_LOW, 128, 64);
	printf("[iotcam]   ssd1306_init                  %s\n",
	       (s == ALP_OK) ? "ok" : "skip (no panel)");

	/* User trigger + status LED via the button_led block.  Board aliases
     * keep the EVK pad choices in generated route metadata instead of
     * hard-coding the E1M/E1M-X namespace in application source. */
	s = alp_button_led_init(&g_trigger,
	                        &(alp_button_led_config_t){
	                            .button_pin_id     = BOARD_PIN_ENCODER_SW,
	                            .led_pin_id        = BOARD_PIN_LED_RED,
	                            .active_low_button = true,
	                        });
	printf("[iotcam]   alp_button_led_init           %s\n",
	       (s == ALP_OK) ? "ok" : "skip (no GPIOs)");

	return 0;
}

/* ------------------------------------------------------------------ */
/* Stage 2 — camera                                                    */
/* ------------------------------------------------------------------ */

static alp_camera_t *g_camera;

static int stage_camera_init(void)
{
	printf("[iotcam] stage 2: camera\n");

	/* TODO(v0.2): open MIPI CSI-2 camera (e.g. OV5640) on the V2N
     * EVK.  RZ/V2N has 2× CSI-2 controllers (4 lanes each); the
     * V2N family routes only `CSI0_*`. */
	g_camera = alp_camera_open(&(alp_camera_config_t){
	    .camera_id = 0,
	    .width     = 320,
	    .height    = 320,
	    .fps       = 15,
	    .format    = ALP_PIXFMT_RGB888,
	});
	if (g_camera == NULL) {
		printf("[iotcam]   alp_camera_open               skip (v0.2 deliverable)\n");
	} else {
		printf("[iotcam]   alp_camera_open               ok\n");
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Stage 3 — classifier (DRP-AI3 on V2N, Ethos-U on AEN)               */
/* ------------------------------------------------------------------ */

static int stage_classifier_load(void)
{
	printf("[iotcam] stage 3: classifier load\n");
	/* TODO(v0.2): on V2N, drop a DRP-AI-translator-compiled model
     * (`.dat`) into models/ and prime the DRP-AI3 driver via the
     * Renesas FSP.  On AEN, this is a Vela-compiled .tflite — see
     * examples/edgeai-vision-aen/.  The unified entry point for
     * both arrives in v0.3 alongside the alp_inference_* surface. */
	printf("[iotcam]   model load                    skip (v0.2 deliverable)\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Stage 4 — networking (Wi-Fi station)                                */
/* ------------------------------------------------------------------ */

static alp_wifi_t *g_wifi;

static int stage_network_connect(void)
{
	printf("[iotcam] stage 4: network\n");
	/* TODO(v0.3): on V2N, the on-module Murata LBEE5HY2FY
	 * (Infineon CYW55513) Wi-Fi/BLE data paths are Linux-owned on
	 * the A55 Yocto slice.  On AEN Zephyr, the same portable surface
	 * resolves to the CC3501E bridge backend.  For this skeleton we
	 * just open the handle.
	 *
	 * v0.1 stub returns NULL — alp_iot_* is documented header-only
	 * surface in v0.1. */
	g_wifi = alp_wifi_open();
	if (g_wifi == NULL) {
		printf("[iotcam]   alp_wifi_open                 skip (v0.3 deliverable)\n");
		return 0;
	}
	alp_status_t s = alp_wifi_connect(g_wifi,
	                                  &(alp_wifi_credentials_t){
	                                      .ssid = "your-ssid", /* replace with your Wi-Fi SSID */
	                                      .psk  = NULL, /* TODO(v0.3): pull from secure store */
	                                  },
	                                  30000);
	printf("[iotcam]   alp_wifi_connect              %s\n",
	       (s == ALP_OK) ? "ok" : "skip (no station)");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Stage 5 — MQTT (over TLS)                                           */
/* ------------------------------------------------------------------ */

static alp_mqtt_t *g_mqtt;

static int stage_mqtt_connect(void)
{
	printf("[iotcam] stage 5: mqtt\n");
	/* TODO(v0.3): wire MbedTLS for MQTT-over-TLS, load the CA from
     * certs/ (see certs/README.md), and connect to the broker
     * configured at build time via DTS or a CONFIG_ option.  The
     * subscribe path picks up commands; publish emits inference
     * results as JSON. */
	g_mqtt = alp_mqtt_open(&(alp_mqtt_config_t){
	    .broker_uri    = "mqtts://broker.example.com:8883",
	    .client_id     = "alp-iotcam-skeleton",
	    .keepalive_s   = 60,
	    .clean_session = true,
	});
	if (g_mqtt == NULL) {
		printf("[iotcam]   alp_mqtt_open                 skip (v0.3 deliverable)\n");
		return 0;
	}
	alp_status_t s = alp_mqtt_connect(g_mqtt, 10000);
	printf("[iotcam]   alp_mqtt_connect              %s\n",
	       (s == ALP_OK) ? "ok" : "skip (no broker)");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Stage 6 — main loop                                                 */
/* ------------------------------------------------------------------ */

static int stage_main_loop(void)
{
	printf("[iotcam] stage 6: main loop\n");

	/* TODO(v0.3): the real loop —
     *
     *   while (running) {
     *       alp_camera_frame_t frame;
     *       alp_camera_capture(g_camera, &frame, 100);
     *       drpai_invoke(frame.data, &result);
     *       alp_camera_release(g_camera, &frame);
     *
     *       const char *json = format_result_json(&result);
     *       alp_mqtt_publish(g_mqtt, "alp/cam/inference",
     *                        (uint8_t *)json, strlen(json),
     *                        ALP_MQTT_QOS_1, false);
     *
     *       lvgl_render_status(&result);   // local UI
     *       alp_mqtt_loop(g_mqtt, 10);     // pump subscribe path
     *   }
     */

	/* v0.1 skeleton just touches the OLED so the SSD1306 path is
     * exercised under emul. */
	if (g_oled.initialised) {
		ssd1306_clear(&g_oled);
		for (uint16_t y = 0; y < 64; y += 8) {
			ssd1306_draw_pixel(&g_oled, 0, y, true);
			ssd1306_draw_pixel(&g_oled, 127, y, true);
		}
		(void)ssd1306_display(&g_oled);
	}

	printf("[iotcam]   main loop                     skip (v0.3 deliverable)\n");
	return 0;
}

/* ------------------------------------------------------------------ */
/* Teardown + entry                                                    */
/* ------------------------------------------------------------------ */

static void stage_teardown(void)
{
	printf("[iotcam] stage 7: teardown\n");
	alp_mqtt_close(g_mqtt);
	alp_wifi_close(g_wifi);
	if (g_camera != NULL) alp_camera_close(g_camera);
	if (g_oled.initialised) ssd1306_deinit(&g_oled);
	alp_button_led_deinit(&g_trigger);
	if (g_sensor_bus != NULL) alp_i2c_close(g_sensor_bus);
}

int main(void)
{
	printf("[iotcam] iot-connected-camera reference — v0.1 skeleton\n");

	if (stage_peripherals_init() != 0) goto done;
	if (stage_camera_init() != 0) goto done;
	if (stage_classifier_load() != 0) goto done;
	if (stage_network_connect() != 0) goto done;
	if (stage_mqtt_connect() != 0) goto done;
	if (stage_main_loop() != 0) goto done;

done:
	stage_teardown();
	printf("[iotcam] done\n");
	return 0;
}
