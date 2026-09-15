/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure DMIC_TRIGGER_START decision logic for the vendored Alif PDM driver
 * (see alif_pdm.c for the full ADR 0017 provenance banner). Split into its
 * own tiny header, alongside alif_pdm_chanmap.h / alif_pdm_burst_plan.h, so
 * tests/unit/alif_pdm_trigger_state can exercise the exact refuse/no-op/
 * proceed decision dmic_alif_pdm_trigger() makes, on the host, with no
 * DEVICE_MMIO/k_msgq/PDM-instance involved.
 *
 * Mirrors dmic_mcux_trigger()'s DMIC_TRIGGER_START precedent (ZEPHYR_BASE
 * zephyr/drivers/audio/dmic_mcux.c:606-616, issue #2133 round 4c):
 *   - refuse (-EIO) a device that either never had a successful
 *     configure() (mcux: state not CONFIGURED/ACTIVE) or has a reported,
 *     not-yet-cleared drop (mcux folds its DMIC_STATE_ERROR into the same
 *     refusal, since ERROR is neither CONFIGURED nor ACTIVE either);
 *   - no-op (return 0 without re-arming) a device that is already running
 *     (mcux: `else if (state != DMIC_STATE_ACTIVE)` skips its start work
 *     the same way when already ACTIVE);
 *   - otherwise proceed to arm the clock and interrupts.
 */
#ifndef ZEPHYR_DRIVERS_AUDIO_ALIF_PDM_TRIGGER_STATE_H_
#define ZEPHYR_DRIVERS_AUDIO_ALIF_PDM_TRIGGER_STATE_H_

#include <stdbool.h>

enum pdm_start_decision {
	PDM_START_REFUSE,  /* -EIO: not configured, or a drop is reported and not yet
			     * cleared by DMIC_TRIGGER_STOP */
	PDM_START_NOOP,    /* already active -- return 0 without re-arming */
	PDM_START_PROCEED, /* arm the clock mode and interrupts */
};

/**
 * @brief Decide what DMIC_TRIGGER_START should do, given the device's
 *        current software state.
 *
 * @param configured True once a successful dmic_alif_pdm_configure() has
 *                    resolved a real clock mode -- i.e.
 *                    pdata->clk_mode != PDM_MODE_MICROPHONE_SLEEP. A
 *                    refused configure() resets clk_mode back to
 *                    PDM_MODE_MICROPHONE_SLEEP, so this stays false until
 *                    the NEXT successful configure().
 * @param overrun    pdata->overrun -- the ISR reported a dropped burst
 *                    (slab exhaustion, delivery-queue overflow, an
 *                    unplannable burst, or a hardware FIFO overflow) and
 *                    DMIC_TRIGGER_STOP has not yet cleared it.
 * @param active     pdata->record_data != 0 -- a capture session is
 *                    already running.
 *
 * @return The decision; see @ref pdm_start_decision.
 */
static inline enum pdm_start_decision pdm_decide_start(bool configured, bool overrun, bool active)
{
	if (!configured || overrun) {
		return PDM_START_REFUSE;
	}
	if (active) {
		return PDM_START_NOOP;
	}
	return PDM_START_PROCEED;
}

#endif /* ZEPHYR_DRIVERS_AUDIO_ALIF_PDM_TRIGGER_STATE_H_ */
