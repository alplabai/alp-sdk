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

ZTEST_SUITE(alp_ahrs, NULL, NULL, NULL, NULL, NULL);
