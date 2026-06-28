/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host unit tests for rotor_speed (tacho + tacholess RPM, BPF) -- native_sim.
 */
#include <math.h>
#include <zephyr/ztest.h>
#include "rotor_speed.h"

ZTEST_SUITE(rotor_speed, NULL, NULL, NULL, NULL, NULL);

ZTEST(rotor_speed, test_bpf_formula)
{
	/* 3 blades at 15 rpm -> BPF = 3 * 15 / 60 = 0.75 Hz. */
	zassert_within((double)rotor_bpf_hz(15.0f, 3), 0.75, 1e-4, "BPF = N*rpm/60");
}

ZTEST(rotor_speed, test_rpm_valid_gate)
{
	zassert_true(rotor_rpm_valid(15.0f), "15 rpm is valid");
	zassert_false(rotor_rpm_valid(0.0f), "0 rpm invalid");
	zassert_false(rotor_rpm_valid(100.0f), "100 rpm invalid");
}

ZTEST(rotor_speed, test_tacho_rpm)
{
	/* 1 pulse/rev, 4,000,000 us between pulses -> 60e6/4e6 = 15 rpm. */
	zassert_within((double)rotor_tacho_rpm(4000000u, 1), 15.0, 0.01, "1 ppr tacho");
	/* 60 ppr encoder at 15 rpm -> interval 66667 us. */
	zassert_within((double)rotor_tacho_rpm(66667u, 60), 15.0, 0.1, "60 ppr tacho");
	zassert_equal(rotor_tacho_rpm(0u, 1), 0.0f, "zero interval guarded");
}

ZTEST(rotor_speed, test_tacholess_recovers_rpm)
{
	/* Build a band-energy envelope amplitude-modulated at BPF=0.75 Hz,
	 * sampled at the 62.5 fps frame rate, N blades = 3 -> rpm 15. */
	const float fr  = ACO_FRAME_RATE_HZ; /* 62.5 */
	const float bpf = 0.75f;
	float       env[256];
	for (int i = 0; i < 256; i++) {
		env[i] = 1.0f + 0.5f * sinf(2.0f * (float)M_PI * bpf * (float)i / fr);
	}
	float rpm = rotor_tacholess_rpm(env, 256, fr, 3);
	zassert_within((double)rpm, 15.0, 1.5, "tacholess RPM within 1.5 of 15");
}
