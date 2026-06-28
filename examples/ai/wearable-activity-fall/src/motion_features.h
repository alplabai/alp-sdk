/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * motion_features -- pure-C windowed accel+gyro feature extraction for the
 * wearable activity / fall example.  Arch-neutral (stdint/math only): builds
 * for native_sim and the Cortex-M55 alike; host-unit-tested.
 */
#ifndef MOTION_FEATURES_H
#define MOTION_FEATURES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOT_WINDOW_N 256
#define MOT_SR_HZ    100.0f
/** 3 accel-axis RMS + 3 gyro-axis RMS + amag_rms + gmag_rms + sma + dom_freq +
 *  jerk_rms + tilt_deg = 12. */
#define MOT_FEATURE_DIM 12

/** One IMU sample: accel in g, gyro in deg/s. */
struct mot_sample {
	float ax, ay, az;
	float gx, gy, gz;
};

struct mot_window_state {
	struct mot_sample s[MOT_WINDOW_N];
	uint16_t          count;
};

struct mot_features {
	float a_rms[3];    /**< per-axis accel AC RMS. */
	float g_rms[3];    /**< per-axis gyro AC RMS. */
	float amag_rms;    /**< RMS of |a| (DC removed). */
	float gmag_rms;    /**< RMS of |gyro| (DC removed). */
	float sma;         /**< signal-magnitude-area: mean(|ax|+|ay|+|az|). */
	float dom_freq_hz; /**< dominant FFT bin of |a| (step cadence). */
	float jerk_rms;    /**< RMS of d|a|/dt. */
	float tilt_deg;    /**< tilt of the mean accel vector from vertical. */
};

void   mot_window_reset(struct mot_window_state *st);
void   mot_window_push(struct mot_window_state *st, struct mot_sample s);
bool   mot_window_full(const struct mot_window_state *st);
void   mot_feat_extract(const struct mot_window_state *st, float sr_hz, struct mot_features *out);
size_t mot_feat_pack(const struct mot_features *f, float *vec, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* MOTION_FEATURES_H */
