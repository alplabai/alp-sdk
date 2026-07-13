/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

/**
 * @file pid.h
 * @brief Alp SDK portable PID controller.
 *
 * A small, caller-owned PID (proportional-integral-derivative) control
 * loop for motor / thermal / power / attitude regulation.  Unlike the
 * DSP chain, a PID needs no pool or handle -- the caller allocates an
 * @ref alp_pid_t (typically static or on the stack), initialises it once
 * with @ref alp_pid_init, and calls @ref alp_pid_step every control tick.
 *
 * The implementation is pure C (no vendor dependency) and builds on every
 * OS target.  It is gated by @c CONFIG_ALP_SDK_PID (the `pid` library knob
 * in `board.yaml`); the profile's HW-accelerator bindings
 * (@c CONFIG_ALP_PID_FPU / @c CONFIG_ALP_PID_TIMER) select a faster path
 * on SoMs that expose one, transparently to this API.
 *
 * Features: output clamping, integrator anti-windup, and
 * derivative-on-measurement (which avoids the "derivative kick" a step
 * change in setpoint would otherwise inject into the D term).
 *
 * @code
 *     static alp_pid_t pid;
 *     const alp_pid_config_t cfg = {
 *         .kp = 0.8f, .ki = 0.2f, .kd = 0.01f,
 *         .out_min = -1.0f, .out_max = 1.0f,
 *         .deriv_on_measurement = true,
 *     };
 *     alp_pid_init(&pid, &cfg);
 *     for (;;) {
 *         float u = alp_pid_step(&pid, setpoint, measured, dt_s);
 *         actuate(u);
 *     }
 * @endcode
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.10 new.  The struct layout is transparent (stack-allocatable)
 *      and may gain fields before v1.0 -- treat it as opaque and go
 *      through the API.  See docs/abi-markers.md.
 */

#ifndef ALP_PID_H
#define ALP_PID_H

#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief PID tuning + limits, supplied to @ref alp_pid_init.
 */
typedef struct {
	float kp; /**< Proportional gain.                              */
	float ki; /**< Integral gain (per second).                     */
	float kd; /**< Derivative gain (seconds).                      */
	/** Output lower clamp.  The step result is saturated to
	 *  `[out_min, out_max]`; set both to 0 to disable clamping. */
	float out_min;
	/** Output upper clamp (see @ref out_min).                    */
	float out_max;
	/** Anti-windup limit on the integrator accumulator (in
	 *  error*time units).  `|integrator|` is clamped to this; pass
	 *  `<= 0` to auto-select half the output span
	 *  (`(out_max - out_min) / 2`). */
	float integ_limit;
	/** When true, the derivative acts on -measurement (avoids the
	 *  derivative kick from a setpoint step); when false, on the
	 *  error.  Steady-state behaviour is identical. */
	bool deriv_on_measurement;
} alp_pid_config_t;

/**
 * @brief PID controller state.  Caller-owned; opaque in practice.
 *
 * Allocate one per control loop (static or stack), initialise with
 * @ref alp_pid_init, then drive with @ref alp_pid_step.  Do not read or
 * write the fields directly -- the layout is @c [ABI-EXPERIMENTAL].
 */
typedef struct {
	alp_pid_config_t cfg;       /**< Copied from @ref alp_pid_init.     */
	float            integ;     /**< Integrator accumulator.            */
	float            prev_meas; /**< Previous measurement (D term).     */
	float            prev_err;  /**< Previous error (D term).           */
	bool             primed;    /**< False until the first step seeds D.*/
} alp_pid_t;

/**
 * @brief Initialise a PID controller from a configuration.
 *
 * Copies @p cfg into @p pid and clears the integrator + derivative
 * history.  Safe to call again at any time to re-tune (it resets state).
 *
 * @param[out] pid  Controller to initialise (non-NULL).
 * @param[in]  cfg  Tuning + limits (non-NULL).  If
 *                  @ref alp_pid_config_t::out_max <
 *                  @ref alp_pid_config_t::out_min the values are treated
 *                  as "no clamp".
 *
 * @return @ref ALP_OK, or @ref ALP_ERR_INVAL if @p pid or @p cfg is NULL.
 */
alp_status_t alp_pid_init(alp_pid_t *pid, const alp_pid_config_t *cfg);

/**
 * @brief Advance the controller one tick and return the control output.
 *
 * Computes `u = Kp*e + Ki*integral(e) - Kd*d`, applies anti-windup to the
 * integrator and saturates @c u to the configured output range.
 *
 * @param[in,out] pid          Controller from @ref alp_pid_init (non-NULL).
 * @param[in]     setpoint     Desired value.
 * @param[in]     measurement  Measured value.
 * @param[in]     dt_s         Time since the previous step, seconds; must
 *                             be > 0 (a non-positive @p dt_s returns 0 and
 *                             does not advance state).
 *
 * @return The saturated control output, or 0.0f if @p pid is NULL or
 *         @p dt_s <= 0.
 */
float alp_pid_step(alp_pid_t *pid, float setpoint, float measurement, float dt_s);

/**
 * @brief Clear the integrator and derivative history (keeps the tuning).
 *
 * Use when the loop has been disengaged (e.g. motors disarmed) so a
 * stale integrator does not slam the output on re-engage.  NULL is a
 * no-op.
 *
 * @param[in,out] pid  Controller, or NULL.
 */
void alp_pid_reset(alp_pid_t *pid);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_PID_H */
