/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #2037: this demo's Phase 11 (sound out -> PDM in) correlation verdict,
 * pulled out of main.c into its own header so it is unit-testable without a
 * board -- same pattern as bmp581_verdict.h and cc3501e_link_verdict.h in
 * this directory.
 *
 * WHAT THIS DOES NOT CLAIM. This is an ENERGY check, not a frequency-domain
 * one: it sums |sample| over a fixed-length PDM capture window and compares
 * two such sums, one taken before the I2S tone starts (room-noise baseline)
 * and one taken while the tone is playing. It cannot tell a 1 kHz tone from
 * line hum or a slammed door, and does not try to -- see the README's Phase
 * 11 row for why a stronger claim (a specific amplitude or frequency
 * response) is not one this bench can honestly make. What it CAN catch: a
 * dead mic, a dead amp, a mis-set mux, or a capture window that never
 * overlapped playback -- every one of those leaves during_energy
 * indistinguishable from baseline_energy, which is exactly what
 * sound_pdm_capture_correlated() refuses to pass.
 *
 * THE FLOOR, not just the ratio. A silent room's baseline can legitimately
 * be near zero (PDM decimator + DC-block leave only a few LSBs of
 * quantization noise), and near-zero times any ratio is still near-zero --
 * so a ratio-only test would pass on a during_energy of 1 against a
 * baseline of 0. SOUND_ENERGY_FLOOR is an absolute minimum during_energy
 * has to clear regardless of how small baseline_energy was, so a capture
 * that stayed silent throughout (mic dead, or nothing ever played) still
 * fails even when the ratio alone would have looked infinite.
 *
 * #2077-follow-up: main.c's Phase 11 used to configure both TAS2563 amps
 * with TAS2563_RX_SLOT_FROM_ADDR on a mono I2S stream, which left BOTH
 * amps silent (one muted by an out-of-frame slot, the other fed a
 * hardcoded-zero right channel by the Alif DW I2S driver's mono path) --
 * see tas2563_rx_channel_t's @warning (include/alp/chips/tas2563.h) and
 * main.c's Phase 11 step 5 comment for the full mechanism. This module's
 * ratio/floor math is agnostic to how many amps or channels are actually
 * driven -- it only sums whatever the PDM mic picked up -- so fixing that
 * bug can only ever RAISE during_energy relative to before (silence ->
 * one or two real speakers), never lower it. SOUND_ENERGY_RATIO_NUM/DEN
 * and SOUND_ENERGY_FLOOR did not need to change for this fix.
 */
#ifndef ALP_EVK_DEMO_SOUND_VERDICT_H
#define ALP_EVK_DEMO_SOUND_VERDICT_H

#include <stdbool.h>
#include <stdint.h>

/* during_energy must be at least this many times baseline_energy... */
#define SOUND_ENERGY_RATIO_NUM 2u
#define SOUND_ENERGY_RATIO_DEN 1u
/* ...AND at least this in absolute terms, so a near-zero baseline can't be
 * "beaten" by a during_energy that is itself still just noise. Sized well
 * below what a few hundred ms of even a quiet played tone sums to at 16-bit
 * PCM (thousands to tens of thousands), but well above the handful of LSBs
 * of decimator noise a silent capture leaves -- see the file header. */
#define SOUND_ENERGY_FLOOR 512u

/*
 * True iff during_energy looks like it captured something that baseline_energy
 * did not -- the one thing this bench can honestly assert about the sound
 * loop (see the file header for what it cannot). Pure integer math: the ratio
 * check is done as a cross-multiply so it never divides, and the whole
 * function takes plain uint32_t sums -- no I2S/PDM handle, no board -- so it
 * is exercised directly in tests/zephyr/chips/src/test_sound_verdict.c
 * without native_sim needing a fake mic.
 */
static inline bool sound_pdm_capture_correlated(uint32_t baseline_energy, uint32_t during_energy)
{
	if (during_energy < SOUND_ENERGY_FLOOR) return false;
	return (uint64_t)during_energy * SOUND_ENERGY_RATIO_DEN >=
	       (uint64_t)baseline_energy * SOUND_ENERGY_RATIO_NUM;
}

#endif /* ALP_EVK_DEMO_SOUND_VERDICT_H */
