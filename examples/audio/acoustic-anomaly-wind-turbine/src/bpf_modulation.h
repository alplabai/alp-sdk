/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * bpf_modulation -- pure-C blade-order analysis of the band-energy envelope.
 * Blade faults modulate audible-band energy at the blade-pass frequency (BPF);
 * a Goertzel evaluated at the *current* BPF makes the feature RPM-invariant.
 * Drivetrain gear-mesh tones are per-frame spectral (in acoustic_features), not
 * here.  Arch-neutral; host-unit-tested.
 */
#ifndef BPF_MODULATION_H
#define BPF_MODULATION_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BPF_ENV_N       256 /* ~4 s at 62.5 fps */
#define BPF_N_HARMONICS 4
#define BPF_FEATURE_DIM (BPF_N_HARMONICS + 1) /* 4 blade orders + modulation_depth */

struct bpf_env_state {
	float    env[BPF_ENV_N];
	uint16_t head;
	uint16_t count;
};

struct bpf_modulation {
	float blade_order_energy[BPF_N_HARMONICS]; /**< normalised energy at k*BPF. */
	float modulation_depth;                    /**< (max-min)/(max+min) of the envelope. */
};

void bpf_env_reset(struct bpf_env_state *st);
/** Append one per-frame energy summary (e.g. the frame's total RMS -- an
 *  ABSOLUTE energy that carries the blade-pass amplitude modulation; do not
 *  pass normalised band energies, whose sum is constant). */
void   bpf_env_push(struct bpf_env_state *st, float frame_energy);
void   bpf_modulation_extract(const struct bpf_env_state *st,
                              float                       bpf_hz,
                              float                       frame_rate_hz,
                              struct bpf_modulation      *out);
size_t bpf_modulation_pack(const struct bpf_modulation *m, float *vec, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* BPF_MODULATION_H */
