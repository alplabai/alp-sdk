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

/** Ring-buffer depth: ~4 s of per-frame energy at 62.5 fps. */
#define BPF_ENV_N 256
/** Number of BPF harmonics evaluated by the Goertzel filter (1x..4x BPF). */
#define BPF_N_HARMONICS 4
/** Feature vector length: BPF_N_HARMONICS blade-order energies + modulation_depth. */
#define BPF_FEATURE_DIM (BPF_N_HARMONICS + 1)

/**
 * Seconds-long ring buffer of per-frame energy summaries.
 * head points to the next write slot; count saturates at BPF_ENV_N.
 */
struct bpf_env_state {
	float    env[BPF_ENV_N]; /**< Per-frame energy ring (absolute amplitude, not normalised). */
	uint16_t head;           /**< Next write position (oldest slot when ring is full). */
	uint16_t count;          /**< Valid entries in [0, BPF_ENV_N]. */
};

struct bpf_modulation {
	float blade_order_energy
	    [BPF_N_HARMONICS];  /**< fraction of envelope AC energy at k*BPF (0..~1). */
	float modulation_depth; /**< (max-min)/(max+min) of the envelope. */
};

/**
 * @brief Reset the envelope ring buffer to the empty state.
 * @param st Ring buffer to clear.
 */
void bpf_env_reset(struct bpf_env_state *st);

/** Append one per-frame energy summary (e.g. the frame's total RMS -- an
 *  ABSOLUTE energy that carries the blade-pass amplitude modulation; do not
 *  pass normalised band energies, whose sum is constant). */
void bpf_env_push(struct bpf_env_state *st, float frame_energy);

/**
 * @brief Extract blade-order energies and modulation depth from the envelope ring.
 *
 * Evaluates a generalised Goertzel at each harmonic k*bpf_hz (k=1..BPF_N_HARMONICS)
 * and computes the peak-to-trough modulation depth of the energy envelope.
 * Passing the live bpf_hz (derived from the current RPM estimate) keeps the
 * Goertzel bins aligned with the rotor regardless of speed variation.
 *
 * @param st           Envelope ring buffer (should hold at least 8 frames).
 * @param bpf_hz       Blade-pass frequency at the current shaft speed (Hz).
 * @param frame_rate_hz Sample rate of the envelope ring (ACO_FRAME_RATE_HZ).
 * @param out          Output; zeroed before filling.
 */
void bpf_modulation_extract(const struct bpf_env_state *st,
                            float                       bpf_hz,
                            float                       frame_rate_hz,
                            struct bpf_modulation      *out);

/**
 * @brief Flatten bpf_modulation into a contiguous float vector.
 *
 * Layout: blade_order_energy[0..BPF_N_HARMONICS-1], modulation_depth.
 *
 * @param m   Source modulation features.
 * @param vec Destination buffer; must hold at least BPF_FEATURE_DIM floats.
 * @param cap Capacity of @p vec in elements.
 * @return Number of elements written (== BPF_FEATURE_DIM), or 0 if cap too small.
 */
size_t bpf_modulation_pack(const struct bpf_modulation *m, float *vec, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* BPF_MODULATION_H */
