/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the portable <alp/pid.h> controller (native_sim).
 * Covers arg guards, P/I/D term behaviour, output clamp + integrator
 * anti-windup, reset, and closed-loop convergence against a simple plant.
 */
#include <zephyr/ztest.h>

#include "alp/pid.h"

ZTEST(alp_pid, test_init_rejects_null)
{
	alp_pid_t              pid;
	const alp_pid_config_t cfg = { .kp = 1.0f };
	zassert_equal(alp_pid_init(NULL, &cfg), ALP_ERR_INVAL, NULL);
	zassert_equal(alp_pid_init(&pid, NULL), ALP_ERR_INVAL, NULL);
	zassert_equal(alp_pid_init(&pid, &cfg), ALP_OK, NULL);
}

ZTEST(alp_pid, test_step_guards_return_zero)
{
	alp_pid_t              pid;
	const alp_pid_config_t cfg = { .kp = 1.0f };
	alp_pid_init(&pid, &cfg);
	zassert_within(alp_pid_step(NULL, 1.0f, 0.0f, 0.01f), 0.0f, 1e-6f, NULL);
	zassert_within(alp_pid_step(&pid, 1.0f, 0.0f, 0.0f), 0.0f, 1e-6f, "dt=0");
	zassert_within(alp_pid_step(&pid, 1.0f, 0.0f, -1.0f), 0.0f, 1e-6f, "dt<0");
}

ZTEST(alp_pid, test_proportional_only)
{
	alp_pid_t              pid;
	const alp_pid_config_t cfg = { .kp = 2.0f, .out_min = -100.0f, .out_max = 100.0f };
	alp_pid_init(&pid, &cfg);
	/* err = 3 - 0 = 3 -> u = kp*err = 6 (no I/D). */
	zassert_within(alp_pid_step(&pid, 3.0f, 0.0f, 0.01f), 6.0f, 1e-4f, NULL);
}

ZTEST(alp_pid, test_output_saturates)
{
	alp_pid_t              pid;
	const alp_pid_config_t cfg = { .kp = 10.0f, .out_min = -1.0f, .out_max = 1.0f };
	alp_pid_init(&pid, &cfg);
	/* kp*err = 50, clamped to out_max = 1. */
	zassert_within(alp_pid_step(&pid, 5.0f, 0.0f, 0.01f), 1.0f, 1e-6f, NULL);
	/* Negative error clamps to out_min. */
	zassert_within(alp_pid_step(&pid, -5.0f, 0.0f, 0.01f), -1.0f, 1e-6f, NULL);
}

ZTEST(alp_pid, test_integrator_windup_clamped)
{
	alp_pid_t pid;
	/* Explicit integ_limit = 3 (below the +/-10 output clamp) so the value
	 * we see is the ANTI-WINDUP limit, not the output saturation.  Ki=1 so
	 * the integral term is bounded to 3. */
	const alp_pid_config_t cfg = {
		.ki = 1.0f, .integ_limit = 3.0f, .out_min = -10.0f, .out_max = 10.0f
	};
	alp_pid_init(&pid, &cfg);
	/* Drive a big sustained error for many steps; integ must not run away. */
	float out = 0.0f;
	for (int i = 0; i < 1000; i++) {
		out = alp_pid_step(&pid, 100.0f, 0.0f, 0.1f);
	}
	zassert_within(out, 3.0f, 1e-3f, "windup not clamped: %f", (double)out);
}

ZTEST(alp_pid, test_reset_clears_integrator)
{
	alp_pid_t              pid;
	const alp_pid_config_t cfg = { .ki = 1.0f, .out_min = -10.0f, .out_max = 10.0f };
	alp_pid_init(&pid, &cfg);
	for (int i = 0; i < 10; i++) {
		(void)alp_pid_step(&pid, 1.0f, 0.0f, 0.1f);
	}
	alp_pid_reset(&pid);
	/* After reset, integ=0, so a zero-error step yields ~0 output. */
	zassert_within(alp_pid_step(&pid, 0.0f, 0.0f, 0.1f), 0.0f, 1e-6f, NULL);
}

ZTEST(alp_pid, test_converges_to_setpoint)
{
	alp_pid_t              pid;
	const alp_pid_config_t cfg = {
		.kp                   = 1.0f,
		.ki                   = 0.5f,
		.kd                   = 0.05f,
		.out_min              = -10.0f,
		.out_max              = 10.0f,
		.deriv_on_measurement = true,
	};
	alp_pid_init(&pid, &cfg);
	/* First-order integrator plant: y += u*dt.  A PI(D) controller must
	 * drive y to the setpoint with zero steady-state error. */
	const float dt = 0.01f, setpoint = 1.0f;
	float       y = 0.0f;
	for (int i = 0; i < 2000; i++) {
		float u = alp_pid_step(&pid, setpoint, y, dt);
		y += u * dt;
	}
	zassert_within(y, setpoint, 1e-2f, "did not converge: y=%f", (double)y);
}

ZTEST_SUITE(alp_pid, NULL, NULL, NULL, NULL, NULL);
