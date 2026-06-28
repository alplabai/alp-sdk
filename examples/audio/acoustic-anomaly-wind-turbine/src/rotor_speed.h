/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * rotor_speed -- pure-C rotor RPM estimation (tacho pulse-interval and
 * tacholess from the band-energy envelope) + blade-pass frequency.
 * Arch-neutral; host-unit-tested.
 */
#ifndef ROTOR_SPEED_H
#define ROTOR_SPEED_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Per-frame rate of the acoustic front end (ACO_SR_HZ / ACO_FRAME_N). */
#define ACO_FRAME_RATE_HZ 62.5f

/**
 * @brief Compute the blade-pass frequency from shaft speed.
 *
 * BPF = n_blades * rpm / 60.  This is the rotor-order fundamental that
 * indexes all blade-pass harmonic analysis (see bpf_modulation.h).
 *
 * @param rpm      Shaft speed in revolutions per minute.
 * @param n_blades Number of rotor blades (typically 3 for utility-scale turbines).
 * @return Blade-pass frequency in Hz.
 */
float rotor_bpf_hz(float rpm, uint8_t n_blades);

/**
 * @brief Plausibility gate for a wind-turbine rotor (3..30 rpm).
 * @param rpm RPM estimate to validate.
 * @return true if rpm is within the operational range; false otherwise.
 */
bool rotor_rpm_valid(float rpm);

/**
 * @brief RPM from a tacho pulse interval (us) and pulses-per-revolution.
 *
 * rev_period_us = pulse_interval_us * pulses_per_rev;
 * rpm = 60e6 / rev_period_us.
 *
 * @param pulse_interval_us Time between consecutive tachometer pulses in microseconds.
 * @param pulses_per_rev    Number of pulses the tachometer emits per shaft revolution.
 * @return Shaft speed in RPM, or 0.0f if either argument is zero.
 */
float rotor_tacho_rpm(uint32_t pulse_interval_us, uint16_t pulses_per_rev);

/**
 * Estimate RPM from the band-energy envelope's blade-pass modulation:
 * autocorrelation peak over the plausible lag range -> BPF -> RPM.
 * The mic-only fallback when no tacho is present.
 *
 * NOTE: the detectable RPM floor is ~ 60 * frame_rate_hz / (n_blades * (n/2));
 * for n=256 at 62.5 fps with 3 blades that is ~10 rpm -- below it the estimate
 * is unreliable even though rotor_rpm_valid() accepts down to 3 rpm.  A real node
 * prefers the tacho path (rotor_tacho_rpm) at low speed.
 */
float rotor_tacholess_rpm(const float *env, size_t n, float frame_rate_hz, uint8_t n_blades);

#ifdef __cplusplus
}
#endif

#endif /* ROTOR_SPEED_H */
