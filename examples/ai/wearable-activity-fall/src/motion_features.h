/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * motion_features -- windowed accel+gyro feature extraction for the wearable
 * activity / fall example.  Arch-neutral: the public API here is stdint/math
 * only, and the implementation's CMSIS-DSP acceleration (magnitude-series
 * stats, jerk RMS, FFT via <alp/dsp.h>) is __has_include-guarded with a
 * portable-C fallback, so the same source builds for native_sim and the
 * Cortex-M55 alike; host-unit-tested.
 *
 * Usage pattern (per 2.56 s window at 100 Hz):
 *
 *   struct mot_window_state win;
 *   mot_window_reset(&win);
 *
 *   while (sensor_ready()) {
 *       mot_window_push(&win, read_imu());
 *       if (mot_window_full(&win)) {
 *           struct mot_features feat;
 *           mot_feat_extract(&win, MOT_SR_HZ, &feat);
 *           // run model or fallback classifier
 *           mot_window_reset(&win);
 *       }
 *   }
 *
 * Feature layout  (MOT_FEATURE_DIM = 12 floats, packed by mot_feat_pack):
 *
 *   slot  0..2  a_rms[0..2]    per-axis accel AC RMS [g]
 *   slot  3..5  g_rms[0..2]    per-axis gyro  AC RMS [deg/s]
 *   slot  6     amag_rms       |a| AC RMS                [g]
 *   slot  7     gmag_rms       |gyro| AC RMS             [deg/s]
 *   slot  8     sma            signal-magnitude area      [g]
 *   slot  9     dom_freq_hz    dominant FFT cadence freq  [Hz]
 *   slot 10     jerk_rms       RMS of d|a|/dt             [g/s]
 *   slot 11     tilt_deg       device tilt from vertical  [deg]
 */
#ifndef MOTION_FEATURES_H
#define MOTION_FEATURES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Constants
 *
 * MOT_WINDOW_N : number of samples per analysis window.  Must be a power of
 *                two (the FFT requires it).  At MOT_SR_HZ = 100 Hz this gives
 *                a 2.56 s window with 0.39 Hz frequency resolution.
 * MOT_SR_HZ    : default sample rate.  Pass to mot_feat_extract() as sr_hz.
 * MOT_FEATURE_DIM : length of the packed feature vector.  See the slot table
 *                in the file header above.
 * --------------------------------------------------------------------------- */
#define MOT_WINDOW_N 256
#define MOT_SR_HZ    100.0f
/** 3 accel-axis RMS + 3 gyro-axis RMS + amag_rms + gmag_rms + sma + dom_freq +
 *  jerk_rms + tilt_deg = 12. */
#define MOT_FEATURE_DIM 12

/* ---------------------------------------------------------------------------
 * Data types
 * --------------------------------------------------------------------------- */

/** One IMU sample: accel in g, gyro in deg/s. */
struct mot_sample {
	float ax, ay, az; /**< accelerometer axes (g). */
	float gx, gy, gz; /**< gyroscope axes (deg/s). */
};

/** Accumulation buffer for one analysis window of IMU samples. */
struct mot_window_state {
	struct mot_sample s[MOT_WINDOW_N]; /**< sample ring; valid indices 0..count-1. */
	uint16_t          count;           /**< number of valid samples currently held. */
};

/*
 * Extracted feature vector.  All fields are computed by mot_feat_extract().
 * "AC RMS" means the RMS of the mean-removed signal (DC subtracted).
 */
struct mot_features {
	float a_rms[3];    /**< per-axis accel AC RMS [g]:  x=0, y=1, z=2. */
	float g_rms[3];    /**< per-axis gyro AC RMS [deg/s]: x=0, y=1, z=2. */
	float amag_rms;    /**< RMS of |a| (DC removed) [g]. */
	float gmag_rms;    /**< RMS of |gyro| (DC removed) [deg/s]. */
	float sma;         /**< signal-magnitude-area: mean(|ax|+|ay|+|az|) [g]. */
	float dom_freq_hz; /**< dominant FFT bin of |a| (step cadence) [Hz]. */
	float jerk_rms;    /**< RMS of d|a|/dt (impact sharpness) [g/s]. */
	float tilt_deg;    /**< tilt of the mean accel vector from vertical [deg]. */
};

/* ---------------------------------------------------------------------------
 * Window management API
 * --------------------------------------------------------------------------- */

/** Reset the window: discard all buffered samples and set count to zero. */
void mot_window_reset(struct mot_window_state *st);

/**
 * Append one IMU sample to the window buffer.  Silently drops the sample
 * when the buffer is full -- caller must check mot_window_full() and reset
 * before pushing the next window's samples.
 */
void mot_window_push(struct mot_window_state *st, struct mot_sample s);

/** Return true when count >= MOT_WINDOW_N and the window is ready to drain. */
bool mot_window_full(const struct mot_window_state *st);

/* ---------------------------------------------------------------------------
 * Feature extraction and packing API
 * --------------------------------------------------------------------------- */

/**
 * Compute all MOT_FEATURE_DIM features from the window @p st and write them
 * to @p out.  Partial windows (count < MOT_WINDOW_N) are accepted; the FFT
 * input is zero-padded to MOT_WINDOW_N.  Pass @p sr_hz = MOT_SR_HZ (100 Hz)
 * for the standard configuration; the jerk and cadence features scale with it.
 */
void mot_feat_extract(const struct mot_window_state *st, float sr_hz, struct mot_features *out);

/**
 * Flatten @p f into the contiguous float vector @p vec (capacity @p cap must
 * be >= MOT_FEATURE_DIM).  The packing order matches the AI model's expected
 * input slot layout (see file header).
 * @return MOT_FEATURE_DIM on success, 0 if cap is too small.
 */
size_t mot_feat_pack(const struct mot_features *f, float *vec, size_t cap);

/* ---------------------------------------------------------------------------
 * Inference API
 *
 * Coarse activity classes.  STAIRS is an AI-only class: the pure-inertial
 * fallback cannot separate stair climbing from walking because:
 *   - Stair cadence (1.2--2.0 Hz) overlaps flat-walk cadence completely.
 *   - amag_rms on stairs overlaps the walking range.
 *   - Distinguishing stair ascent/descent requires altitude change -- a
 *     barometer, not an accelerometer.
 * mot_activity_fallback() maps STAIRS to WALK safely; see its implementation.
 * --------------------------------------------------------------------------- */

/** Coarse activity classes.  STAIRS is an AI-only class -- the deterministic
 *  fallback cannot separate it from WALK without a barometer. */
typedef enum {
	ACT_IDLE   = 0, /**< sedentary: near-zero AC motion. */
	ACT_WALK   = 1, /**< walking (also: stairs in fallback mode). */
	ACT_RUN    = 2, /**< running: high cadence + high amag_rms. */
	ACT_STAIRS = 3, /**< stair climbing / descent (AI model only). */
	ACT_CLASS_COUNT
} mot_activity_t;

/** Activity classification result from the model or the fallback. */
struct mot_verdict {
	mot_activity_t cls;
	float          confidence; /**< 0..1 */
};

/** Deterministic idle/walk/run classifier over the feature vector.  Runs when no
 *  AI model is loaded.  Never emits STAIRS (maps that case to WALK). */
struct mot_verdict mot_activity_fallback(const struct mot_features *f);

/** Stable upper-case class name for logging and display.  Returns "UNKNOWN" for
 *  out-of-range values. */
const char *mot_activity_name(mot_activity_t c);

#ifdef __cplusplus
}
#endif

#endif /* MOTION_FEATURES_H */
