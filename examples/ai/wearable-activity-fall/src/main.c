/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * wearable-activity-fall
 * ======================
 *
 * Body-worn IMU edge node.  Pipeline:
 *
 *   ICM-42670 accel+gyro (I2C, 100 Hz, +/-16 g) --every sample-->
 *     fall_detect (3-phase free-fall -> impact -> stillness state machine)
 *   ICM-42670 --256-sample window (2.56 s)-->
 *     motion_features (RMS/SMA/cadence/jerk/tilt) -> <alp/inference.h>
 *       activity classifier (idle/walk/run/stairs) + deterministic fallback
 *     --> one WACT record per window; falls flagged immediately.
 *
 * Honest scope: body-worn motion; coarse activity + fall detection.  NOT
 * medical-grade, not a certified fall alarm.  The model is a stub (see
 * models/README.md); with no model the deterministic fallback runs.
 */
#include <math.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alp/board.h"
#include "alp/chips/icm42670.h"
#include "alp/inference.h"
#include "alp/peripheral.h"

#include "fall_detect.h"
#include "motion_features.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

LOG_MODULE_REGISTER(wact, LOG_LEVEL_INF);

#define ICM_ACCEL_LSB_PER_G  2048.0f /* +/-16 g full-scale. */
#define ICM_GYRO_LSB_PER_DPS 16.4f   /* +/-2000 dps full-scale. */
#define IMU_I2C_ADDR         ICM42670_I2C_ADDR_HIGH
#define N_WINDOWS            8

/* 1-byte stub so alp_inference_open's non-NULL contract is met; an unusable
 * tensor forces the deterministic fallback.  See models/README.md. */
static const uint8_t s_model[] = { 0x00 };

/* Synthetic motion for native_sim: one window per activity, plus an injected
 * fall in window 5. */
static struct mot_sample synth_sample(int window, int i)
{
	float             t = (float)i / MOT_SR_HZ;
	struct mot_sample s = {
		.ax = 0.0f, .ay = 0.0f, .az = 1.0f, .gx = 0.0f, .gy = 0.0f, .gz = 0.0f
	};
	switch (window % 4) {
	case 0: /* idle */
		break;
	case 1: /* walk ~2 Hz */
		s.az = 1.0f + 0.3f * sinf(2.0f * (float)M_PI * 2.0f * t);
		s.gx = 10.0f * sinf(2.0f * (float)M_PI * 2.0f * t);
		break;
	case 2: /* run ~3 Hz */
		s.az = 1.0f + 1.2f * sinf(2.0f * (float)M_PI * 3.0f * t);
		s.gx = 40.0f * sinf(2.0f * (float)M_PI * 3.0f * t);
		break;
	default: /* stairs-ish: walk cadence + tilt */
		s.az = 0.9f + 0.3f * sinf(2.0f * (float)M_PI * 1.8f * t);
		s.ax = 0.3f;
		break;
	}
	return s;
}

/* Overlay a free-fall -> impact -> stillness sequence on window 5's |a|. */
static float synth_fall_amag(int i)
{
	if (i < 20) {
		return 1.0f; /* normal */
	} else if (i < 35) {
		return 0.2f; /* free-fall (15 samples) */
	} else if (i < 37) {
		return 5.0f; /* impact */
	}
	return 1.0f; /* stillness */
}

static struct mot_verdict classify(alp_inference_t *inf, const struct mot_features *f)
{
	if (inf != NULL) {
		float vec[MOT_FEATURE_DIM];
		(void)mot_feat_pack(f, vec, MOT_FEATURE_DIM);
		alp_inference_tensor_t in = { 0 };
		if (alp_inference_get_input(inf, 0, &in) == ALP_OK && in.dtype == ALP_INFERENCE_DTYPE_F32 &&
		    in.data != NULL && in.size_bytes >= sizeof(vec)) {
			memcpy(in.data, vec, sizeof(vec));
			if (alp_inference_invoke(inf) == ALP_OK) {
				alp_inference_tensor_t out = { 0 };
				if (alp_inference_get_output(inf, 0, &out) == ALP_OK &&
				    out.dtype == ALP_INFERENCE_DTYPE_F32 && out.data != NULL &&
				    out.size_bytes >= ACT_CLASS_COUNT * sizeof(float)) {
					const float *sc   = (const float *)out.data;
					int          best = 0;
					float        bv   = sc[0];
					for (int k = 1; k < ACT_CLASS_COUNT; k++) {
						if (sc[k] > bv) {
							bv   = sc[k];
							best = k;
						}
					}
					return (struct mot_verdict){ (mot_activity_t)best, bv };
				}
			}
		}
	}
	return mot_activity_fallback(f);
}

int main(void)
{
	static icm42670_t              imu;
	static struct mot_window_state win;
	static struct fall_state       fall;
	bool                           imu_ok = false;

	fall_reset(&fall);

	/* BOARD_I2C_SENSORS resolves to the shared sensor I2C bus on whichever
	 * EVK is targeted.  alp_i2c_open() takes a config struct; failure is
	 * tolerated -- the loop falls back to synthetic motion. */
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = BOARD_I2C_SENSORS,
	    .bitrate_hz = 400000,
	});
	if (bus != NULL && icm42670_init(&imu, bus, IMU_I2C_ADDR) == ALP_OK &&
	    icm42670_set_accel(&imu, ICM42670_ODR_100_HZ, ICM42670_ACCEL_FS_16G) == ALP_OK &&
	    icm42670_set_gyro(&imu, ICM42670_ODR_100_HZ, ICM42670_GYRO_FS_2000_DPS) == ALP_OK) {
		imu_ok = true;
	} else {
		LOG_WRN("ICM-42670 unavailable; using synthetic motion");
	}

	alp_inference_t *inf = alp_inference_open(&(alp_inference_config_t){
	    .backend    = ALP_INFERENCE_BACKEND_AUTO,
	    .format     = ALP_INFERENCE_MODEL_TFLITE,
	    .model_data = s_model,
	    .model_size = sizeof(s_model),
	});

	printk("# WACT,t_s,activity,confidence,fall,impact_g\n");

	for (int w = 0; w < N_WINDOWS; w++) {
		mot_window_reset(&win);
		bool  fall_fired = false;
		float impact_g   = 0.0f;

		for (int i = 0; i < MOT_WINDOW_N; i++) {
			struct mot_sample s;
			if (imu_ok) {
				icm42670_axes_t a, g;
				if (icm42670_read_accel(&imu, &a) == ALP_OK &&
				    icm42670_read_gyro(&imu, &g) == ALP_OK) {
					s.ax = (float)a.x / ICM_ACCEL_LSB_PER_G;
					s.ay = (float)a.y / ICM_ACCEL_LSB_PER_G;
					s.az = (float)a.z / ICM_ACCEL_LSB_PER_G;
					s.gx = (float)g.x / ICM_GYRO_LSB_PER_DPS;
					s.gy = (float)g.y / ICM_GYRO_LSB_PER_DPS;
					s.gz = (float)g.z / ICM_GYRO_LSB_PER_DPS;
				} else {
					s = synth_sample(w, i);
				}
			} else {
				s = synth_sample(w, i);
			}

			/* Window 5 (native_sim) overlays a fall on the |a| magnitude. */
			float amag;
			if (!imu_ok && w == 5) {
				amag = synth_fall_amag(i);
				s.az = amag; /* drive the window features too */
				s.ax = 0.0f;
				s.ay = 0.0f;
			} else {
				amag = sqrtf(s.ax * s.ax + s.ay * s.ay + s.az * s.az);
			}

			mot_window_push(&win, s);

			float ig = 0.0f;
			if (fall_push(&fall, amag, MOT_SR_HZ, &ig)) {
				fall_fired = true;
				impact_g   = ig;
			}
		}

		struct mot_features f;
		mot_feat_extract(&win, MOT_SR_HZ, &f);
		struct mot_verdict v = classify(inf, &f);

		printk("WACT,%.2f,%s,%.2f,%d,%.1f\n",
		       (double)(w * 2.56f),
		       mot_activity_name(v.cls),
		       (double)v.confidence,
		       fall_fired ? 1 : 0,
		       (double)impact_g);
	}

	if (inf != NULL) {
		alp_inference_close(inf);
	}
	if (imu_ok) {
		icm42670_deinit(&imu);
	}
	alp_i2c_close(bus);
	printk("[wact] done\n");
	return 0;
}
