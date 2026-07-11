/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable Madgwick AHRS (IMU variant) -- see <alp/ahrs.h>.  Pure C
 * (libm only); compiled on every OS target behind CONFIG_ALP_SDK_AHRS.
 *
 * Algorithm: Sebastian Madgwick's gradient-descent orientation filter
 * (xioTechnologies MadgwickAHRS, IMU update).  We use libm 1/sqrtf for
 * the normalisations rather than the original's fast-inverse-sqrt so the
 * result is accurate + portable; SoMs with an FPU / TMU CORDIC bind a
 * faster path via the library profile.
 */
#include <math.h>

#include <alp/ahrs.h>

alp_status_t alp_ahrs_init(alp_ahrs_t *ahrs, const alp_ahrs_config_t *cfg)
{
	if (ahrs == NULL) {
		return ALP_ERR_INVAL;
	}
	ahrs->q0   = 1.0f;
	ahrs->q1   = 0.0f;
	ahrs->q2   = 0.0f;
	ahrs->q3   = 0.0f;
	ahrs->beta = (cfg != NULL && cfg->beta > 0.0f) ? cfg->beta : ALP_AHRS_DEFAULT_BETA;
	return ALP_OK;
}

void alp_ahrs_reset(alp_ahrs_t *ahrs)
{
	if (ahrs == NULL) {
		return;
	}
	ahrs->q0 = 1.0f;
	ahrs->q1 = 0.0f;
	ahrs->q2 = 0.0f;
	ahrs->q3 = 0.0f;
}

void alp_ahrs_update_imu(alp_ahrs_t *ahrs,
                         float       gx,
                         float       gy,
                         float       gz,
                         float       ax,
                         float       ay,
                         float       az,
                         float       dt_s)
{
	if (ahrs == NULL || dt_s <= 0.0f) {
		return;
	}
	float q0 = ahrs->q0, q1 = ahrs->q1, q2 = ahrs->q2, q3 = ahrs->q3;

	/* Rate of change of the quaternion from the gyroscope. */
	float qDot1 = 0.5f * (-q1 * gx - q2 * gy - q3 * gz);
	float qDot2 = 0.5f * (q0 * gx + q2 * gz - q3 * gy);
	float qDot3 = 0.5f * (q0 * gy - q1 * gz + q3 * gx);
	float qDot4 = 0.5f * (q0 * gz + q1 * gy - q2 * gx);

	/* Apply the accelerometer correction only when it carries signal
	 * (a zero vector -- free-fall / dropout -- would divide by zero;
	 * that sample falls back to pure gyro integration). */
	if (!(ax == 0.0f && ay == 0.0f && az == 0.0f)) {
		/* Normalise the accelerometer measurement. */
		float recipNorm = 1.0f / sqrtf(ax * ax + ay * ay + az * az);
		ax *= recipNorm;
		ay *= recipNorm;
		az *= recipNorm;

		/* Auxiliary variables to avoid repeated arithmetic. */
		float _2q0 = 2.0f * q0, _2q1 = 2.0f * q1, _2q2 = 2.0f * q2, _2q3 = 2.0f * q3;
		float _4q0 = 4.0f * q0, _4q1 = 4.0f * q1, _4q2 = 4.0f * q2;
		float _8q1 = 8.0f * q1, _8q2 = 8.0f * q2;
		float q0q0 = q0 * q0, q1q1 = q1 * q1, q2q2 = q2 * q2, q3q3 = q3 * q3;

		/* Gradient-descent corrective step (objective = align the
		 * estimated gravity with the measured accel). */
		float s0 = _4q0 * q2q2 + _2q2 * ax + _4q0 * q1q1 - _2q1 * ay;
		float s1 = _4q1 * q3q3 - _2q3 * ax + 4.0f * q0q0 * q1 - _2q0 * ay - _4q1 + _8q1 * q1q1 +
		           _8q1 * q2q2 + _4q1 * az;
		float s2 = 4.0f * q0q0 * q2 + _2q0 * ax + _4q2 * q3q3 - _2q3 * ay - _4q2 + _8q2 * q1q1 +
		           _8q2 * q2q2 + _4q2 * az;
		float s3 = 4.0f * q1q1 * q3 - _2q1 * ax + 4.0f * q2q2 * q3 - _2q2 * ay;
		float step_norm = s0 * s0 + s1 * s1 + s2 * s2 + s3 * s3;

		/* Apply the correction only when the gradient step is non-zero.
		 * When the estimate already matches gravity exactly (e.g. level
		 * + identity quaternion) the step is 0, and normalising it would
		 * be 1/sqrt(0) -> inf -> NaN; skip it (no correction needed). */
		if (step_norm > 0.0f) {
			recipNorm = 1.0f / sqrtf(step_norm);
			qDot1 -= ahrs->beta * s0 * recipNorm;
			qDot2 -= ahrs->beta * s1 * recipNorm;
			qDot3 -= ahrs->beta * s2 * recipNorm;
			qDot4 -= ahrs->beta * s3 * recipNorm;
		}
	}

	/* Integrate to yield the new quaternion, then renormalise. */
	q0 += qDot1 * dt_s;
	q1 += qDot2 * dt_s;
	q2 += qDot3 * dt_s;
	q3 += qDot4 * dt_s;
	float recipNorm = 1.0f / sqrtf(q0 * q0 + q1 * q1 + q2 * q2 + q3 * q3);
	ahrs->q0        = q0 * recipNorm;
	ahrs->q1        = q1 * recipNorm;
	ahrs->q2        = q2 * recipNorm;
	ahrs->q3        = q3 * recipNorm;
}

void alp_ahrs_euler(const alp_ahrs_t *ahrs, float *roll_deg, float *pitch_deg, float *yaw_deg)
{
	if (ahrs == NULL) {
		return;
	}
	const float q0 = ahrs->q0, q1 = ahrs->q1, q2 = ahrs->q2, q3 = ahrs->q3;
	const float rad2deg = 57.29577951308232f;

	if (roll_deg != NULL) {
		*roll_deg = atan2f(2.0f * (q0 * q1 + q2 * q3), 1.0f - 2.0f * (q1 * q1 + q2 * q2)) * rad2deg;
	}
	if (pitch_deg != NULL) {
		float s = 2.0f * (q0 * q2 - q3 * q1);
		if (s > 1.0f) {
			s = 1.0f; /* clamp for asinf domain near +/-90 deg */
		}
		if (s < -1.0f) {
			s = -1.0f;
		}
		*pitch_deg = asinf(s) * rad2deg;
	}
	if (yaw_deg != NULL) {
		*yaw_deg = atan2f(2.0f * (q0 * q3 + q1 * q2), 1.0f - 2.0f * (q2 * q2 + q3 * q3)) * rad2deg;
	}
}
