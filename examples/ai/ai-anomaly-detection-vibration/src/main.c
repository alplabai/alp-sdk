/*
 * Copyright 2026 Alp Lab AB
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
 *   - FS_2G sensitivity: both chips are 16384 LSB/g at ±2 g (datasheet
 *     DS-000451 table 3 for ICM-42670; AN5192 table 2 for LSM6DSO).
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
 *      -- yes, flip `som.sku` in board.yaml to E1M-V2M101; the
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
 * - The native_sim build can't talk to a real ICM-42670, so the IMU
 *   open returns NOSUPPORT and the loop fills the window with a
 *   SYNTHESIZED deterministic vibration signal (healthy sinusoid +
 *   an injected fault transient on alternate windows) instead of
 *   zeros, so the score is meaningful rather than a flat 0.0000.
 * - With the 1-byte stub model the real inference output is empty,
 *   so the demo falls back to a transparent crest-factor heuristic
 *   and labels each line src=model|heuristic so the score is never
 *   presented as something it isn't.  A real model replaces both.
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
#define WINDOW_SAMPLES 256u
#define IMU_ODR ICM42670_ODR_800_HZ
#define IMU_FS ICM42670_ACCEL_FS_2G /* 16384 LSB / g at ±2 g */

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

/* Raw int16 LSB to float "g".  Both the ICM-42670 and the LSM6DSO
 * have identical ±2 g full-scale sensitivity: 16384 LSB/g
 * (ICM-42670: datasheet DS-000451 table 3; LSM6DSO: AN5192 table 2).
 * We compute the magnitude in g because the anomaly model was trained
 * against g-magnitude features in the v0.5 reference pipeline. */
static inline float accel_magnitude_g(const icm42670_axes_t *a)
{
    const float lsb_per_g = 16384.0f; /* ICM-42670 FS_2G sensitivity (= LSM6DSO FS_2G) */
    const float fx        = (float)a->x / lsb_per_g;
    const float fy        = (float)a->y / lsb_per_g;
    const float fz        = (float)a->z / lsb_per_g;
    return sqrtf(fx * fx + fy * fy + fz * fz);
}

/* Synthesize a deterministic vibration window when no IMU is
 * present (native_sim / missing chip).  A flat zero-fill would make
 * every score read 0.0000 and teach the reader nothing, so instead
 * we generate a repeatable signal: a 1 g baseline plus a dominant
 * sinusoid (the motor's running vibration), and -- on a fixed subset
 * of windows -- an injected high-amplitude transient (a simulated
 * bearing-fault "knock").  `iter` selects healthy vs faulty windows
 * so the demo shows the score moving between the two.
 *
 * This is SIMULATED sensor data, not a recording; it exists so the
 * pipeline produces a meaningful, varying score offline.  On real
 * silicon fill_window() reads the actual ICM-42670 instead. */
static void synthesize_window(float *out, size_t n, int iter)
{
    /* Every 2nd window carries the injected fault transient. */
    const bool faulty = (iter % 2) == 1;
    for (size_t i = 0; i < n; i++) {
        /* ~1 g gravity baseline + a 60 Hz-ish running sinusoid
         * (period chosen against the 256-sample window for a clean
         * repeat). */
        float v = 1.0f + 0.15f * sinf((2.0f * 3.14159265f * 8.0f * (float)i) / (float)n);
        if (faulty) {
            /* Injected impulsive transient: a decaying knock part-way
             * through the window.  Big enough to dominate the crest
             * factor / RMS so a vibration-anomaly model (or the
             * heuristic fallback below) flags it. */
            if (i >= n / 3 && i < n / 3 + 16) {
                float k = (float)(i - n / 3);
                v += 1.2f * expf(-k / 4.0f);
            }
        }
        out[i] = v;
    }
}

/* Fill the window from the IMU.  When the IMU handle is NULL --
 * native_sim or a missing chip -- we fall back to a synthesized
 * deterministic waveform (see synthesize_window) so the framing
 * path still runs AND the score is meaningful rather than a flat
 * zero. */
static void fill_window(icm42670_t *imu, float *out, size_t n, int iter)
{
    if (imu == NULL) {
        synthesize_window(out, n, iter);
        return;
    }
    for (size_t i = 0; i < n; i++) {
        icm42670_axes_t a = { 0 };
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
    alp_inference_tensor_t in = { 0 };
    if (alp_inference_get_input(inf, 0, &in) != ALP_OK) {
        return;
    }
    if (in.data == NULL || in.size_bytes == 0) {
        return; /* Stub backend; nothing to fill. */
    }
    if (in.dtype == ALP_INFERENCE_DTYPE_F32) {
        size_t bytes = n * sizeof(float);
        if (bytes > in.size_bytes) bytes = in.size_bytes;
        memcpy(in.data, win, bytes);
    } else if (in.dtype == ALP_INFERENCE_DTYPE_INT8) {
        int8_t *q   = (int8_t *)in.data;
        size_t  cap = (in.size_bytes < n) ? in.size_bytes : n;
        for (size_t i = 0; i < cap; i++) {
            /* y = round(x / scale) + zero_point, clamp to int8. */
            float   v  = win[i] / (in.scale > 0.0f ? in.scale : 1.0f);
            int32_t qv = (int32_t)(v + 0.5f) + in.zero_point;
            if (qv > 127) qv = 127;
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
    alp_inference_tensor_t out = { 0 };
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

/* Transparent heuristic anomaly score, used ONLY when the real
 * inference backend produced nothing usable (the v0.5 stub model, or
 * native_sim with no NPU).  This is NOT the trained model -- it's a
 * simple crest-factor measure (peak / RMS): a clean sinusoid sits
 * near ~1.4, while an impulsive bearing-fault transient pushes it
 * higher.  Normalised so a healthy window lands near 0 and a faulty
 * one well above it, giving the demo a meaningful, varying score
 * offline.  A real deployment ignores this entirely and uses the
 * model output from read_anomaly_score(). */
static float heuristic_anomaly_score(const float *win, size_t n)
{
    if (n == 0) return 0.0f;
    float sumsq = 0.0f;
    float peak  = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float a = fabsf(win[i]);
        sumsq += win[i] * win[i];
        if (a > peak) peak = a;
    }
    float rms = sqrtf(sumsq / (float)n);
    if (rms <= 0.0f) return 0.0f;
    float crest = peak / rms;    /* ~1.41 for a pure sinusoid. */
    float score = crest - 1.41f; /* clean window -> ~0. */
    return score < 0.0f ? 0.0f : score;
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
    icm42670_t  imu   = { 0 };
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
        fill_window(imu_p, s_window, WINDOW_SAMPLES, iter);

        /* Try the real model first.  read_anomaly_score() returns 0
         * when the backend is the v0.5 stub (rank-0 output) or no
         * NPU is present -- in that case fall back to the transparent
         * crest-factor heuristic so the demo still shows a meaningful,
         * varying score offline.  `from_model` tells the reader which
         * path produced the number. */
        float score      = 0.0f;
        bool  from_model = false;
        if (inf != NULL) {
            copy_window_into_input(inf, s_window, WINDOW_SAMPLES);
            if (alp_inference_invoke(inf) == ALP_OK) {
                score      = read_anomaly_score(inf);
                from_model = (score != 0.0f);
            }
        }
        if (!from_model) {
            score = heuristic_anomaly_score(s_window, WINDOW_SAMPLES);
        }

        /* Dashboard line -- one row per window.  Threshold +
         * hysteresis logic for "raise an alarm" is the
         * application owner's call; v0.5 just publishes the raw
         * score so the customer can plot it and pick a cutoff.
         * `src` flags whether the score came from the trained model
         * or the heuristic fallback so the log is never misleading. */
        printf("[anomaly] window=%d score=%.4f src=%s\n", iter, (double)score,
               from_model ? "model" : "heuristic");

        /* On native_sim fill_window() returns instantly (synthesized,
         * no IMU pacing), so running all four windows is cheap and
         * lets the reader see the score alternate between the healthy
         * and injected-fault windows.  On HiL each pass blocks ~308 ms
         * at the 833 Hz ODR. */
    }

    alp_inference_close(inf);
    if (imu_p) icm42670_deinit(imu_p);
    alp_i2c_close(i2c);

    printf("[anomaly] done\n");
    return 0;
}
