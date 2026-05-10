/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * EdgeAI vision-AEN reference application — v0.1 skeleton.
 *
 * The full pipeline is the v0.2 deliverable.  This file ships the
 * shape of that pipeline so:
 *   1. Newcomers can read the intended data flow today.
 *   2. CI catches breakage of v0.1 SDK surfaces early (the init
 *      stages are real and exercise <alp/peripheral.h>,
 *      <alp/chips/ssd1306.h>, <alp/chips/lsm6dso.h>,
 *      <alp/chips/button_led.h>).
 *   3. Each v0.2 step has a concrete TODO it slots into.
 *
 * v0.1 prints a one-line status per stage and exits cleanly under
 * native_sim / on a real EVK.  v0.2 implementations replace the
 * TODOs in-place — the surrounding scaffolding stays.
 */

#include <stdio.h>

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp/e1m_pinout.h"
#include "alp/chips/lsm6dso.h"
#include "alp/chips/ssd1306.h"
#include "alp/chips/button_led.h"
#include "alp/camera.h"
#include "alp/inference.h"

/* ------------------------------------------------------------------ */
/* Pipeline stage 1 — peripherals                                      */
/* ------------------------------------------------------------------ */

static alp_i2c_t       *g_sensor_bus;
static lsm6dso_t        g_imu;
static ssd1306_t        g_oled;
static alp_button_led_t g_trigger;

static int              stage_peripherals_init(void)
{
    printf("[edgeai] stage 1: peripherals\n");

    g_sensor_bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = ALP_E1M_I2C0,
        .bitrate_hz = 400000,
    });
    if (g_sensor_bus == NULL) {
        printf("[edgeai]   alp_i2c_open(I2C0) failed\n");
        return -1;
    }
    printf("[edgeai]   alp_i2c_open(I2C0)            ok\n");

    /* SSD1306 status overlay.  On a real EVK the panel sits behind
     * level shifters on a separate I²C bus (DSI_I2C), wired through
     * the IO expander; v0.1 skeleton uses the sensor bus to keep
     * the host build single-bus. */
    alp_status_t s = ssd1306_init(&g_oled, g_sensor_bus, SSD1306_I2C_ADDR_LOW, 128, 64);
    printf("[edgeai]   ssd1306_init                  %s\n",
           (s == ALP_OK) ? "ok" : "skip (no panel)");

    s = lsm6dso_init(&g_imu, g_sensor_bus, LSM6DSO_I2C_ADDR_LOW);
    printf("[edgeai]   lsm6dso_init                  %s\n", (s == ALP_OK) ? "ok" : "skip (no IMU)");

    s = alp_button_led_init(&g_trigger, &(alp_button_led_config_t){
                                            .button_pin_id     = ALP_E1M_GPIO_IO0,
                                            .led_pin_id        = ALP_E1M_GPIO_IO1,
                                            .active_low_button = true,
                                        });
    printf("[edgeai]   alp_button_led_init           %s\n",
           (s == ALP_OK) ? "ok" : "skip (no GPIOs)");

    return 0;
}

/* ------------------------------------------------------------------ */
/* Pipeline stage 2 — camera                                           */
/* ------------------------------------------------------------------ */

static alp_camera_t *g_camera;

static int           stage_camera_init(void)
{
    printf("[edgeai] stage 2: camera\n");

    /* TODO(v0.2): open MIPI CSI-2 camera (e.g. OV5640) on the EVK
     * via <alp/camera.h>.  The v0.1 surface header is in place but
     * every alp_camera_* call returns ALP_ERR_NOSUPPORT until the
     * Zephyr-video integration lands in v0.2. */
    g_camera = alp_camera_open(&(alp_camera_config_t){
        .camera_id = 0,
        .width     = 224, /* MobileNetV2 input size */
        .height    = 224,
        .fps       = 15,
        .format    = ALP_PIXFMT_RGB888,
    });
    if (g_camera == NULL) {
        printf("[edgeai]   alp_camera_open               skip (v0.2 deliverable)\n");
        return 0;
    }
    printf("[edgeai]   alp_camera_open               ok\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Pipeline stage 3 — model load                                       */
/* ------------------------------------------------------------------ */

static alp_inference_t *g_model;

static int              stage_model_load(void)
{
    printf("[edgeai] stage 3: model load\n");
    /* The SDK exposes a unified inference surface via <alp/inference.h>.
     * On AEN-Zephyr (CONFIG_ALP_SDK_INFERENCE_TFLM=y +
     * CONFIG_ALP_SDK_INFERENCE_ETHOS_U=y) this opens a Vela-compiled
     * MobileNetV2 with the Ethos-U op resolver wired in; on native_sim
     * (no TFLM) the wrapper falls back to NOSUPPORT and the example
     * keeps running with the model load skipped.
     *
     * The model bytes ship under models/ -- empty in the v0.2
     * scaffold, populated when the Vela toolchain run lands.  The
     * 16-byte placeholder below lets the open() call validate
     * input parameters today. */
    static const uint8_t   s_placeholder[16] = {0xDE, 0xAD, 0xBE, 0xEF};
    alp_inference_config_t cfg               = {0};
    cfg.model_data                           = s_placeholder;
    cfg.model_size                           = sizeof(s_placeholder);
    cfg.format                               = ALP_INFERENCE_MODEL_VELA;
    cfg.backend                              = ALP_INFERENCE_BACKEND_AUTO;
    /* arena: NULL -> backend's built-in default. */
    g_model = alp_inference_open(&cfg);
    if (g_model == NULL) {
        printf("[edgeai]   alp_inference_open            skip (last_err=%d)\n",
               (int)alp_last_error());
    } else {
        printf("[edgeai]   alp_inference_open(AUTO)      ok (%zu inputs / %zu outputs)\n",
               alp_inference_num_inputs(g_model), alp_inference_num_outputs(g_model));
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Pipeline stage 4 — inference loop                                   */
/* ------------------------------------------------------------------ */

static int stage_inference_loop(void)
{
    printf("[edgeai] stage 4: inference loop\n");

    /* TODO(v0.2): the real loop —
     *
     *   while (running) {
     *       alp_camera_frame_t frame;
     *       alp_camera_capture(g_camera, &frame, 100);
     *
     *       // Pre-processing (normalise + crop) via <alp/math.h>
     *       preprocess_to_tensor(frame.data, &input_tensor);
     *       alp_camera_release(g_camera, &frame);
     *
     *       // Inference on Ethos-U55-HP
     *       ethosu_invoke(&input_tensor, &output_tensor);
     *
     *       // Top-1 class + confidence to OLED
     *       const char *label;
     *       float       conf;
     *       argmax_top1(&output_tensor, &label, &conf);
     *       overlay_to_oled(&g_oled, label, conf);
     *
     *       // Optional: blink LED on capture, IMU tilt → viewport
     *       alp_button_led_toggle(&g_trigger);
     *   }
     *
     * v0.1 skeleton just runs one synthetic iteration so the build
     * proves out and CI sees a clean exit. */

    if (g_oled.initialised) {
        ssd1306_clear(&g_oled);
        for (uint16_t x = 0; x < 128; x += 8) {
            ssd1306_draw_pixel(&g_oled, x, 0, true);
            ssd1306_draw_pixel(&g_oled, x, 63, true);
        }
        (void)ssd1306_display(&g_oled);
    }

    printf("[edgeai]   inference loop                skip (v0.2 deliverable)\n");
    return 0;
}

/* ------------------------------------------------------------------ */
/* Pipeline stage 5 — teardown                                         */
/* ------------------------------------------------------------------ */

static void stage_teardown(void)
{
    printf("[edgeai] stage 5: teardown\n");
    alp_inference_close(g_model);
    if (g_camera != NULL) alp_camera_close(g_camera);
    if (g_oled.initialised) ssd1306_deinit(&g_oled);
    if (g_imu.initialised) lsm6dso_deinit(&g_imu);
    alp_button_led_deinit(&g_trigger);
    if (g_sensor_bus != NULL) alp_i2c_close(g_sensor_bus);
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    printf("[edgeai] vision-aen reference — v0.1 skeleton\n");

    if (stage_peripherals_init() != 0) goto done;
    if (stage_camera_init() != 0) goto done;
    if (stage_model_load() != 0) goto done;
    if (stage_inference_loop() != 0) goto done;

done:
    stage_teardown();
    printf("[edgeai] done\n");
    return 0;
}
