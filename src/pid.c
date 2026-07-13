/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable PID controller -- see <alp/pid.h>.  Pure C, no vendor deps;
 * compiled on every OS target behind CONFIG_ALP_SDK_PID.
 */
#include <alp/pid.h>

alp_status_t alp_pid_init(alp_pid_t *pid, const alp_pid_config_t *cfg)
{
	if (pid == NULL || cfg == NULL) {
		return ALP_ERR_INVAL;
	}
	pid->cfg       = *cfg;
	pid->integ     = 0.0f;
	pid->prev_meas = 0.0f;
	pid->prev_err  = 0.0f;
	pid->primed    = false;
	return ALP_OK;
}

void alp_pid_reset(alp_pid_t *pid)
{
	if (pid == NULL) {
		return;
	}
	pid->integ     = 0.0f;
	pid->prev_meas = 0.0f;
	pid->prev_err  = 0.0f;
	pid->primed    = false;
}

float alp_pid_step(alp_pid_t *pid, float setpoint, float measurement, float dt_s)
{
	if (pid == NULL || dt_s <= 0.0f) {
		return 0.0f;
	}
	const alp_pid_config_t *c   = &pid->cfg;
	const float             err = setpoint - measurement;

	/* Integrate the error, then clamp the accumulator (anti-windup).
	 * integ_limit <= 0 auto-selects half the output span. */
	pid->integ += err * dt_s;
	float ilim = c->integ_limit;
	if (ilim <= 0.0f) {
		ilim = (c->out_max > c->out_min) ? ((c->out_max - c->out_min) * 0.5f) : 0.0f;
	}
	if (ilim > 0.0f) {
		if (pid->integ > ilim) {
			pid->integ = ilim;
		}
		if (pid->integ < -ilim) {
			pid->integ = -ilim;
		}
	}

	/* Derivative: on measurement (default, avoids the setpoint-step kick)
	 * or on error.  The first step has no history, so D is zero. */
	float deriv = 0.0f;
	if (pid->primed) {
		deriv = c->deriv_on_measurement ? ((measurement - pid->prev_meas) / dt_s)
		                                : ((err - pid->prev_err) / dt_s);
	}
	pid->prev_meas = measurement;
	pid->prev_err  = err;
	pid->primed    = true;

	/* u = Kp*e + Ki*integral(e) +/- Kd*derivative.  Sign: at a constant
	 * setpoint d(err) = -d(meas), so deriv-on-measurement subtracts and
	 * deriv-on-error adds -- both damp the response identically. */
	float out = c->kp * err + c->ki * pid->integ;
	out += c->deriv_on_measurement ? (-c->kd * deriv) : (c->kd * deriv);

	/* Saturate to the output range (out_max <= out_min disables clamp). */
	if (c->out_max > c->out_min) {
		if (out < c->out_min) {
			out = c->out_min;
		}
		if (out > c->out_max) {
			out = c->out_max;
		}
	}
	return out;
}
