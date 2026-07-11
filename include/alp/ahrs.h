/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file ahrs.h
 * @brief Alp SDK portable AHRS (attitude and heading reference system).
 *
 * A caller-owned Madgwick orientation filter that fuses a 3-axis gyro
 * with a 3-axis accelerometer into a drift-corrected orientation
 * quaternion (the IMU variant; a magnetometer-aided MARG variant may be
 * added later).  The accelerometer pins roll/pitch against gravity so the
 * estimate does not drift the way a raw gyro integrator does.
 *
 * Like @ref alp_pid_t this needs no pool or handle: allocate an
 * @ref alp_ahrs_t (static or stack), @ref alp_ahrs_init it once, then
 * call @ref alp_ahrs_update_imu every sample and read Euler angles with
 * @ref alp_ahrs_euler.  Pure C (libm only); builds on every OS target,
 * gated by @c CONFIG_ALP_SDK_AHRS (the `madgwick_ahrs` `board.yaml`
 * library knob).  Faster HW paths (FPU / TMU CORDIC) bind transparently
 * on SoMs that expose them.
 *
 * @code
 *     static alp_ahrs_t ahrs;
 *     alp_ahrs_init(&ahrs, &(alp_ahrs_config_t){ .beta = 0.1f });
 *     for (;;) {
 *         // gyro in rad/s; accel in any consistent unit (it is normalised)
 *         alp_ahrs_update_imu(&ahrs, gx, gy, gz, ax, ay, az, dt_s);
 *         float roll, pitch, yaw;
 *         alp_ahrs_euler(&ahrs, &roll, &pitch, &yaw);
 *     }
 * @endcode
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.10 new.  The struct layout is transparent (stack-allocatable)
 *      and may gain fields before v1.0 -- treat it as opaque and go
 *      through the API.  See docs/abi-markers.md.
 */

#ifndef ALP_AHRS_H
#define ALP_AHRS_H

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Default Madgwick filter gain (rad/s) -- a reasonable starting point
 *  for a typical MEMS IMU; raise for faster accel correction (more
 *  noise), lower for smoother output (slower drift correction). */
#define ALP_AHRS_DEFAULT_BETA 0.1f

/**
 * @brief AHRS configuration, supplied to @ref alp_ahrs_init.
 */
typedef struct {
	/** Madgwick gain (rad/s).  Pass `<= 0` to use
	 *  @ref ALP_AHRS_DEFAULT_BETA. */
	float beta;
} alp_ahrs_config_t;

/**
 * @brief AHRS filter state.  Caller-owned; opaque in practice.
 *
 * The orientation is held as a unit quaternion `(q0, q1, q2, q3)` mapping
 * the sensor frame to the earth frame.  Do not read or write the fields
 * directly -- the layout is @c [ABI-EXPERIMENTAL]; use @ref alp_ahrs_euler.
 */
typedef struct {
	float q0;   /**< Quaternion scalar part.      */
	float q1;   /**< Quaternion x.                 */
	float q2;   /**< Quaternion y.                 */
	float q3;   /**< Quaternion z.                 */
	float beta; /**< Filter gain (from config).  */
} alp_ahrs_t;

/**
 * @brief Initialise the filter to the identity orientation.
 *
 * Sets the quaternion to `(1,0,0,0)` (level, facing +x) and copies the
 * gain from @p cfg.  Safe to call again to re-seed.
 *
 * @param[out] ahrs  Filter to initialise (non-NULL).
 * @param[in]  cfg   Configuration; if NULL, @ref ALP_AHRS_DEFAULT_BETA is
 *                   used.
 *
 * @return @ref ALP_OK, or @ref ALP_ERR_INVAL if @p ahrs is NULL.
 */
alp_status_t alp_ahrs_init(alp_ahrs_t *ahrs, const alp_ahrs_config_t *cfg);

/**
 * @brief Advance the filter one sample (gyro + accelerometer fusion).
 *
 * Runs one Madgwick IMU update: integrates the gyro and applies a
 * gradient-descent correction toward the gravity direction measured by
 * the accelerometer, then renormalises the quaternion.
 *
 * @param[in,out] ahrs  Filter from @ref alp_ahrs_init (non-NULL).
 * @param[in]     gx    Gyro X, rad/s.
 * @param[in]     gy    Gyro Y, rad/s.
 * @param[in]     gz    Gyro Z, rad/s.
 * @param[in]     ax    Accel X (any consistent unit; the vector is
 *                      normalised, so raw counts are fine).
 * @param[in]     ay    Accel Y.
 * @param[in]     az    Accel Z.
 * @param[in]     dt_s  Time since the previous update, seconds; must be
 *                      > 0 (a non-positive @p dt_s is ignored).
 *
 * @note If the accelerometer vector is zero (free-fall or a dropout) the
 *       update falls back to gyro-only integration for that sample.
 */
void alp_ahrs_update_imu(alp_ahrs_t *ahrs,
                         float       gx,
                         float       gy,
                         float       gz,
                         float       ax,
                         float       ay,
                         float       az,
                         float       dt_s);

/**
 * @brief Read the current orientation as aerospace Euler angles.
 *
 * Converts the internal quaternion to intrinsic Z-Y-X (yaw-pitch-roll)
 * Euler angles in DEGREES.  Any output pointer may be NULL to skip it.
 *
 * @param[in]  ahrs       Filter (non-NULL); NULL leaves outputs untouched.
 * @param[out] roll_deg   Roll about X, degrees, or NULL.
 * @param[out] pitch_deg  Pitch about Y, degrees `[-90, 90]`, or NULL.
 * @param[out] yaw_deg    Yaw about Z, degrees, or NULL.
 */
void alp_ahrs_euler(const alp_ahrs_t *ahrs, float *roll_deg, float *pitch_deg, float *yaw_deg);

/**
 * @brief Reset the orientation to identity (keeps the gain).  NULL is a
 *        no-op.
 *
 * @param[in,out] ahrs  Filter, or NULL.
 */
void alp_ahrs_reset(alp_ahrs_t *ahrs);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_AHRS_H */
