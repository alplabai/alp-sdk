/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * ai-anomaly-detection-vibration
 * ==============================
 *
 * Predictive-maintenance edge-AI demo on E1M-AEN (or V2N).
 *
 *   ┌──────────────────┐   I2C0   ┌────────────────────────┐
 *   │ E1M SoM          │ ◀────────│ ICM-42670 6-axis IMU   │
 *   │ + Cortex-M55 HP  │  (0x68   │ (populated on BOTH     │
 *   │   or M33 SM      │   or 69) │  E1M EVK + E1M-X EVK)  │
 *   └────────┬─────────┘          └────────────────────────┘
 *            │  256-sample sliding window of accel-mag values
 *            ▼
 *      ┌──────────────────────┐
 *      │  TFLM dispatch via   │  -- 1D-CNN anomaly model
 *      │  <alp/inference.h>   │     compiled with Vela for
 *      │  BACKEND_AUTO        │     ethos-u55-128 on AEN,
 *      │                      │     DEEPX DX-M1 on V2N,
 *      │                      │     reference kernels on
 *      │                      │     native_sim.
 *      └─────────┬────────────┘
 *                │  per-window anomaly score
 *                ▼
 *           ┌──────────┐
 *           │ printk   │  -- one line per window, log line
 *           │ dashboard│     becomes the customer's input for
 *           └──────────┘     their MQTT / OPC-UA gateway.
 *
 *
 * ── Why the ICM-42670 instead of the LSM6DSO ──────────────────
 *
 * The ICM-42670-P is populated on BOTH the E1M EVK (35x35 mm, AEN
 * family) and the E1M-X EVK (45x65 mm, V2N family).  The LSM6DSO
 * is fitted only on the E1M EVK.  Since this demo targets both
 * EVKs via BOARD_I2C_SENSORS and the portable <alp/board.h> layer,
 * we use the ICM-42670 driver for cross-EVK portability.
 *
 * ICM-42670-P vs LSM6DSO for this demo:
 *   - Same 6-axis accel + gyro, I2C + SPI interface.
 *   - ODR closest to LSM6DSO's 833 Hz is ICM42670_ODR_800_HZ (800 Hz).
 *   - FS_2G sensitivity: ICM-42670 = 2048 LSB/g (vs 16384 on LSM6DSO).
 *   - API shape: icm42670_init / icm42670_set_accel / icm42670_read_accel
 *     / icm42670_deinit -- same lifecycle pattern as the LSM6DSO driver.
 *
 * EVK I2C address:
 *   E1M EVK (UG-E1M-001):  AP_AD0 = high  → 0x69
 *   E1M-X EVK:             AP_AD0 default → 0x68 (check your carrier)
 *   This example uses ICM42670_I2C_ADDR_HIGH (0x69) to match the
 *   E1M EVK strap.  Override by redefining IMU_I2C_ADDR if your
 *   carrier wires AD0 low.
 *
 *
 * ── Why this demo matters ────────────────────────────────────
 *
 * Three customer questions get answered side-by-side:
 *   1. "Can we monitor a motor / pump / spindle for early-failure
 *      signs without piping raw vibration to the cloud?" -- yes,
 *      the model runs entirely on the on-die NPU; only the score
 *      leaves the device.
 *   2. "Is it cheap enough to deploy on every machine?" -- yes,
 *      the Ethos-U + M55 HP combo on AEN701 drops to a single
 *      microamp average when the inference path is clock-gated
 *      between windows.  Battery-powered nodes run for months.
 *   3. "Does the same source target V2N for the AI-PLC story?"
 *      -- yes, flip `som.sku` in board.yaml to E1M-V2N201; the
 *      §D.lib loader routes ALP_INFERENCE_BACKEND_AUTO to the
 *      DEEPX DX-M1 driver shim.
 *
 *
 * ── What's stubbed in v0.5 ────────────────────────────────────
 *
 * - The real anomaly model isn't checked in -- customers drop a
 *   Vela-compiled `.tflite` into `models/` and point the loader
 *   at it.  The placeholder model bytes here let the framing
 *   path compile + run on native_sim and HiL.
 * - The native_sim build can't talk to a real ICM-42670, so the
 *   IMU open returns NOSUPPORT; the loop fills the window with
 *   zeros and the framing path still exercises end-to-end.
 * - The "dashboard" is a single printf line per window.  Real
 *   integrations forward the score via <alp/iot.h> MQTT or a
 *   field-bus (OPC-UA / Modbus / EtherCAT).
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "alp/peripheral.h"

/* BOARD_I2C_SENSORS is a portable alias that resolves to the shared
 * sensor I2C bus on whichever EVK is being targeted:
 *   E1M EVK  (AEN)  → E1M_I2C0
 *   E1M-X EVK (V2N) → E1M_X_I2C0
 * Include via <alp/board.h>; ALP_BOARD_* is emitted by the build
 * system from the board.yaml preset. */
#include "alp/board.h"
#include "alp/inference.h"
#include "alp/chips/icm42670.h"

LOG_MODULE_REGISTER(anomaly, LOG_LEVEL_INF);

/* ───────── Sliding-window sizing ─────────
 *
 * 256 samples at the ICM-42670's 800 Hz ODR is ~320 ms of vibration
 * history per window -- long enough to span the dominant mechanical
 * resonance of a typical small AC motor / pump, short enough that an
 * anomalous transient still gets caught inside one window.
 *
 * The original demo used the LSM6DSO at 833 Hz ODR; the ICM-42670's
 * nearest standard rate is 800 Hz (ICM42670_ODR_800_HZ).  The ~4%
 * difference in window duration (320 ms vs 308 ms) doesn't affect the
 * framing logic but matters when retraining: rebuild your anomaly
 * model with data sampled at 800 Hz to get accurate score calibration.
 */
#define WINDOW_SAMPLES   256u
#define IMU_ODR          ICM42670_ODR_800_HZ
#define IMU_FS           ICM42670_ACCEL_FS_2G   /* 2048 LSB / g */

/* I2C address of the ICM-42670 on the E1M EVK (AP_AD0 = high → 0x69).
 * Override by redefining this before including the header if your
 * carrier board wires AD0 low (→ ICM42670_I2C_ADDR_LOW = 0x68). */
#ifndef IMU_I2C_ADDR
#define IMU_I2C_ADDR ICM42670_I2C_ADDR_HIGH
#endif

/* Per-sample magnitude buffer -- the input the 1D-CNN expects.
 * Storing magnitudes (not raw x/y/z triples) gives the model an
 * orientation-invariant input + keeps the tensor 1-D.  TODO: a
 * real anomaly model may want the raw triaxial signal; that's a
 * config knob for the customer's retraining pipeline. */
static float s_window[WINDOW_SAMPLES];

/* Placeholder Vela-compiled model bytes.  Real customers replace
 * with `#include "models/anomaly_1dcnn_vela.h"` generated by
 * upstream Vela.  Kept as a 1-byte stub so the framing path
 * compiles + the SDK's NULL-tolerant alp_inference_open returns
 * a usable handle (the dispatcher tolerates "no model" on the
 * v0.5 stub backend). */
static const uint8_t s_model[] = { 0x00 };

/* Tensor arena.  Sized to match `default_arena_kib: 128` in
 * board.yaml; small because a 1D-CNN over 256 samples is much
 * lighter than a vision model.  TODO: when the real model lands
 * in v0.6, re-measure with `vela --show-cpu-operations` and
 * bump if needed. */
static uint8_t s_arena[128 * 1024] __aligned(16);

/* ───────── Sample-loop helpers ───────── */

/* Raw int16 LSB to float "g".  ICM-42670 at FS_2G: 2048 LSB per g
 * (datasheet DS-000451, table 3.  Note: the LSM6DSO at FS_2G uses
 * 16384 LSB/g -- a different sensitivity; this constant must match
 * the chip being used.)
 * We compute the magnitude in g because the anomaly model was trained
 * against g-magnitude features in the v0.5 reference pipeline. */
static inline float accel_magnitude_g(const icm42670_axes_t *a)
{
    const float lsb_per_g = 2048.0f;   /* ICM-42670 FS_2G sensitivity */
    const float fx = (float)a->x / lsb_per_g;
    const float fy = (float)a->y / lsb_per_g;
    const float fz = (float)a->z / lsb_per_g;
    return sqrtf(fx * fx + fy * fy + fz * fz);
}

/* Fill the window from the IMU.  When the IMU handle is NULL --
 * native_sim or a missing chip -- we zero-fill so the framing
 * path still runs; the customer sees a flat anomaly score that
 * confirms the model is being invoked even without a real sensor. */
static void fill_window(icm42670_t *imu, float *out, size_t n)
{
    if (imu == NULL) {
        memset(out, 0, n * sizeof(*out));
        return;
    }
    for (size_t i = 0; i < n; i++) {
        icm42670_axes_t a = {0};
        if (icm42670_read_accel(imu, &a) != ALP_OK) {
            out[i] = 0.0f;
            continue;
        }
        out[i] = accel_magnitude_g(&a);
        /* Pace the loop at the chip's 800 Hz ODR.  Real
         * deployments wire the ICM-42670 INT1 line to a GPIO IRQ
         * for jitter-free sample-ready dispatch; v0.5 polls. */
        k_usleep(1250);
    }
}

/* Copy a float window into the model's input tensor.  Quantises
 * to int8 when the model expects int8 (typical for Vela / DEEPX
 * compiled paths), or memcpy-as-float32 when the model is f32.
 * Falls through silently if the tensor descriptor doesn't fit
 * the window -- v0.5 stub backend returns rank-0 tensors. */
static void copy_window_into_input(alp_inference_t *inf, const float *win, size_t n)
{
    alp_inference_tensor_t in = {0};
    if (alp_inference_get_input(inf, 0, &in) != ALP_OK) {
        return;
    }
    if (in.data == NULL || in.size_bytes == 0) {
        return;  /* Stub backend; nothing to fill. */
    }
    if (in.dtype == ALP_INFERENCE_DTYPE_F32) {
        size_t bytes = n * sizeof(float);
        if (bytes > in.size_bytes) bytes = in.size_bytes;
        memcpy(in.data, win, bytes);
    } else if (in.dtype == ALP_INFERENCE_DTYPE_INT8) {
        int8_t *q = (int8_t *)in.data;
        size_t cap = (in.size_bytes < n) ? in.size_bytes : n;
        for (size_t i = 0; i < cap; i++) {
            /* y = round(x / scale) + zero_point, clamp to int8. */
            float v = win[i] / (in.scale > 0.0f ? in.scale : 1.0f);
            int32_t qv = (int32_t)(v + 0.5f) + in.zero_point;
            if (qv >  127) qv =  127;
            if (qv < -128) qv = -128;
            q[i] = (int8_t)qv;
        }
    }
    /* TODO(v0.6): handle UINT8 / INT16 once the customer-facing
     * Vela quantisation matrix is finalised. */
}

/* Extract the anomaly score from the output tensor.  Single
 * scalar by convention for 1D-CNN-based anomaly heads;
 * de-quantise if the model emits int8. */
static float read_anomaly_score(alp_inference_t *inf)
{
    alp_inference_tensor_t out = {0};
    if (alp_inference_get_output(inf, 0, &out) != ALP_OK || out.data == NULL) {
        return 0.0f;
    }
    if (out.dtype == ALP_INFERENCE_DTYPE_F32 && out.size_bytes >= sizeof(float)) {
        float v;
        memcpy(&v, out.data, sizeof(v));
        return v;
    }
    if (out.dtype == ALP_INFERENCE_DTYPE_INT8 && out.size_bytes >= 1) {
        int8_t q = ((const int8_t *)out.data)[0];
        return ((float)q - (float)out.zero_point) * out.scale;
    }
    return 0.0f;
}

int main(void)
{
    printf("[anomaly] alp-sdk vibration anomaly detection demo (ICM-42670)\n");

    /* I2C bring-up.  BOARD_I2C_SENSORS resolves to the on-board sensor
     * bus on whichever EVK is being targeted.  400 kHz is comfortable
     * for the ICM-42670's 1 MHz max.  Failure tolerated -- the loop
     * falls back to a zero-fill window so native_sim still runs. */
    alp_i2c_t *i2c = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = BOARD_I2C_SENSORS, /* E1M EVK: E1M_I2C0; E1M-X EVK: E1M_X_I2C0 */
        .bitrate_hz = 400000,
    });
    if (i2c == NULL) {
        printf("[anomaly] I2C open failed -- running with synthetic window\n");
    }

    /* IMU bring-up.  WHO_AM_I check happens inside icm42670_init;
     * we treat failure as "no IMU" and carry on. */
    icm42670_t imu = {0};
    icm42670_t *imu_p = NULL;
    if (i2c != NULL) {
        if (icm42670_init(&imu, i2c, IMU_I2C_ADDR) == ALP_OK) {
            (void)icm42670_set_accel(&imu, IMU_ODR, IMU_FS);
            imu_p = &imu;
        } else {
            printf("[anomaly] ICM-42670 WHO_AM_I failed -- synthetic window\n");
        }
    }

    /* Inference bring-up.  AUTO routes to the on-die NPU on real
     * silicon, falls back to CPU on native_sim.  TODO: when the
     * v0.6 real-model artifact lands, switch format to
     * ALP_INFERENCE_MODEL_VELA for AEN / NX9101 + DXNN for V2N
     * (the loader-emitted preset will set this). */
    alp_inference_t *inf = alp_inference_open(&(alp_inference_config_t){
        .backend     = ALP_INFERENCE_BACKEND_AUTO,
        .format      = ALP_INFERENCE_MODEL_TFLITE,
        .model_data  = s_model,
        .model_size  = sizeof(s_model),
        .arena       = s_arena,
        .arena_bytes = sizeof(s_arena),
    });
    if (inf == NULL) {
        printf("[anomaly] inference_open returned NULL -- v0.5 stub backend\n");
    }

    /* Steady-state loop -- one inference per window.  On HiL the
     * window-fill blocks at the 800 Hz ODR (~320 ms per pass); on
     * native_sim it returns instantly and we exit after one
     * iteration so the twister harness sees a clean "done". */
    for (int iter = 0; iter < 4; iter++) {
        fill_window(imu_p, s_window, WINDOW_SAMPLES);

        float score = 0.0f;
        if (inf != NULL) {
            copy_window_into_input(inf, s_window, WINDOW_SAMPLES);
            if (alp_inference_invoke(inf) == ALP_OK) {
                score = read_anomaly_score(inf);
            }
        }

        /* Dashboard line -- one row per window.  Threshold +
         * hysteresis logic for "raise an alarm" is the
         * application owner's call; v0.5 just publishes the raw
         * score so the customer can plot it and pick a cutoff. */
        printf("[anomaly] window=%d score=%.4f\n", iter, (double)score);

#ifdef CONFIG_BOARD_NATIVE_SIM
        if (iter == 0) break;  /* one iteration is enough for framing test */
#endif
    }

    alp_inference_close(inf);
    if (imu_p) icm42670_deinit(imu_p);
    alp_i2c_close(i2c);

    printf("[anomaly] done\n");
    return 0;
}
