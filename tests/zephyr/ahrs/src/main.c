/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the portable <alp/ahrs.h> Madgwick filter (native_sim).
 * Covers arg guards, identity init, and gyro+accel fusion converging the
 * estimated orientation to the accelerometer-implied tilt.
 */
#include <math.h>

#include <zephyr/ztest.h>

#include "alp/ahrs.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

ZTEST(alp_ahrs, test_init_rejects_null)
{
	alp_ahrs_t ahrs;
	zassert_equal(alp_ahrs_init(NULL, NULL), ALP_ERR_INVAL, NULL);
	zassert_equal(alp_ahrs_init(&ahrs, NULL), ALP_OK, NULL);
}

ZTEST(alp_ahrs, test_identity_is_level)
{
	alp_ahrs_t ahrs;
	alp_ahrs_init(&ahrs, NULL);
	float roll = 9.0f, pitch = 9.0f, yaw = 9.0f;
	alp_ahrs_euler(&ahrs, &roll, &pitch, &yaw);
	zassert_within(roll, 0.0f, 1e-3f, "roll %f", (double)roll);
	zassert_within(pitch, 0.0f, 1e-3f, "pitch %f", (double)pitch);
	zassert_within(yaw, 0.0f, 1e-3f, "yaw %f", (double)yaw);
}

ZTEST(alp_ahrs, test_update_guards)
{
	alp_ahrs_t ahrs;
	alp_ahrs_init(&ahrs, NULL);
	/* NULL + non-positive dt must not advance / crash. */
	alp_ahrs_update_imu(NULL, 1, 1, 1, 0, 0, 1, 0.01f);
	alp_ahrs_update_imu(&ahrs, 1, 1, 1, 0, 0, 1, 0.0f);
	alp_ahrs_update_imu(&ahrs, 1, 1, 1, 0, 0, 1, -1.0f);
	float roll = 0, pitch = 0;
	alp_ahrs_euler(&ahrs, &roll, &pitch, NULL);
	zassert_within(roll, 0.0f, 1e-3f, NULL); /* still identity */
	zassert_within(pitch, 0.0f, 1e-3f, NULL);
}

ZTEST(alp_ahrs, test_level_accel_stays_level)
{
	alp_ahrs_t ahrs;
	alp_ahrs_init(&ahrs, &(alp_ahrs_config_t){ .beta = 0.2f });
	/* Zero gyro, gravity straight down (sensor z): must stay ~level. */
	for (int i = 0; i < 2000; i++) {
		alp_ahrs_update_imu(&ahrs, 0, 0, 0, 0.0f, 0.0f, 1.0f, 0.01f);
	}
	float roll = 0, pitch = 0;
	alp_ahrs_euler(&ahrs, &roll, &pitch, NULL);
	zassert_within(roll, 0.0f, 2.0f, "roll %f", (double)roll);
	zassert_within(pitch, 0.0f, 2.0f, "pitch %f", (double)pitch);
}

ZTEST(alp_ahrs, test_converges_to_tilt)
{
	alp_ahrs_t ahrs;
	alp_ahrs_init(&ahrs, &(alp_ahrs_config_t){ .beta = 0.3f });
	/* A 20-deg tilt about the Y axis: accel = (sin20, 0, cos20).  The
	 * filter must converge |pitch| -> 20 deg with roll staying ~0. */
	const float th  = 20.0f * (float)M_PI / 180.0f;
	const float axf = sinf(th), azf = cosf(th);
	for (int i = 0; i < 4000; i++) {
		alp_ahrs_update_imu(&ahrs, 0, 0, 0, axf, 0.0f, azf, 0.01f);
	}
	float roll = 0, pitch = 0;
	alp_ahrs_euler(&ahrs, &roll, &pitch, NULL);
	zassert_within(fabsf(pitch), 20.0f, 3.0f, "pitch %f (want ~20)", (double)pitch);
	zassert_within(roll, 0.0f, 3.0f, "roll %f (want ~0)", (double)roll);
}

ZTEST(alp_ahrs, test_reset_rejects_null)
{
	/* alp_ahrs_reset() takes the caller-owned struct directly (no
	 * alp_*_open() handle, no backend, no alp_status_t return) -- the
	 * only guard is the NULL check; proven here by "does not crash". */
	alp_ahrs_reset(NULL);
}

ZTEST(alp_ahrs, test_reset_returns_to_identity_after_drift)
{
	alp_ahrs_t ahrs;
	alp_ahrs_init(&ahrs, &(alp_ahrs_config_t){ .beta = 0.3f });

	/* Drive the filter away from identity with a sustained tilt. */
	const float th  = 20.0f * (float)M_PI / 180.0f;
	const float axf = sinf(th), azf = cosf(th);
	for (int i = 0; i < 500; i++) {
		alp_ahrs_update_imu(&ahrs, 0, 0, 0, axf, 0.0f, azf, 0.01f);
	}
	float pitch = 0;
	alp_ahrs_euler(&ahrs, NULL, &pitch, NULL);
	zassert_true(fabsf(pitch) > 1.0f,
	             "precondition: filter must have drifted off level (pitch %f)",
	             (double)pitch);

	alp_ahrs_reset(&ahrs);

	/* Post-reset state: the quaternion is back to identity. */
	zassert_within(ahrs.q0, 1.0f, 1e-6f, NULL);
	zassert_within(ahrs.q1, 0.0f, 1e-6f, NULL);
	zassert_within(ahrs.q2, 0.0f, 1e-6f, NULL);
	zassert_within(ahrs.q3, 0.0f, 1e-6f, NULL);

	float roll = 9, pitch2 = 9, yaw = 9;
	alp_ahrs_euler(&ahrs, &roll, &pitch2, &yaw);
	zassert_within(roll, 0.0f, 1e-3f, "roll %f", (double)roll);
	zassert_within(pitch2, 0.0f, 1e-3f, "pitch %f", (double)pitch2);
	zassert_within(yaw, 0.0f, 1e-3f, "yaw %f", (double)yaw);
}

ZTEST(alp_ahrs, test_post_reset_update_matches_fresh_init)
{
	/* Reset must leave the filter fully usable, not just zero the
	 * quaternion -- a reset filter driven by the same input sequence
	 * as a freshly alp_ahrs_init()'d one must converge identically. */
	alp_ahrs_t fresh, reset_ahrs;
	alp_ahrs_init(&fresh, &(alp_ahrs_config_t){ .beta = 0.3f });
	alp_ahrs_init(&reset_ahrs, &(alp_ahrs_config_t){ .beta = 0.3f });

	/* Perturb reset_ahrs only, then bring it back with alp_ahrs_reset(). */
	alp_ahrs_update_imu(&reset_ahrs, 0.1f, 0.1f, 0.1f, 0.0f, 0.0f, 1.0f, 0.01f);
	alp_ahrs_reset(&reset_ahrs);

	const float th  = 20.0f * (float)M_PI / 180.0f;
	const float axf = sinf(th), azf = cosf(th);
	for (int i = 0; i < 500; i++) {
		alp_ahrs_update_imu(&fresh, 0, 0, 0, axf, 0.0f, azf, 0.01f);
		alp_ahrs_update_imu(&reset_ahrs, 0, 0, 0, axf, 0.0f, azf, 0.01f);
	}
	zassert_within(fresh.q0, reset_ahrs.q0, 1e-4f, NULL);
	zassert_within(fresh.q1, reset_ahrs.q1, 1e-4f, NULL);
	zassert_within(fresh.q2, reset_ahrs.q2, 1e-4f, NULL);
	zassert_within(fresh.q3, reset_ahrs.q3, 1e-4f, NULL);
}

ZTEST_SUITE(alp_ahrs, NULL, NULL, NULL, NULL, NULL);
