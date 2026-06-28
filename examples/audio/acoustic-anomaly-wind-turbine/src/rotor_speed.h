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

/** Blade-pass frequency: BPF = n_blades * rpm / 60. */
float rotor_bpf_hz(float rpm, uint8_t n_blades);

/** Plausibility gate for a wind-turbine rotor (3..30 rpm). */
bool rotor_rpm_valid(float rpm);

/** RPM from a tacho pulse interval (us) and pulses-per-revolution. */
float rotor_tacho_rpm(uint32_t pulse_interval_us, uint16_t pulses_per_rev);

/**
 * Estimate RPM from the band-energy envelope's blade-pass modulation:
 * autocorrelation peak over the plausible lag range -> BPF -> RPM.
 * The mic-only fallback when no tacho is present.
 */
float rotor_tacholess_rpm(const float *env, size_t n, float frame_rate_hz, uint8_t n_blades);

#ifdef __cplusplus
}
#endif

#endif /* ROTOR_SPEED_H */
