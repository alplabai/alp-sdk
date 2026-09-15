/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure DMIC_TRIGGER_START / dmic_alif_pdm_configure() session-state decision
 * logic for the vendored Alif PDM driver (see alif_pdm.c for the full ADR
 * 0017 provenance banner). Split into its own tiny header, alongside
 * alif_pdm_chanmap.h / alif_pdm_burst_plan.h, so tests/unit/
 * alif_pdm_trigger_state can exercise the exact refuse/no-op/proceed
 * decisions dmic_alif_pdm_trigger() and dmic_alif_pdm_configure() make, on
 * the host, with no DEVICE_MMIO/k_msgq/PDM-instance involved.
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

/**
 * @brief Decide whether dmic_alif_pdm_configure() may proceed.
 *
 * Mirrors dmic_mcux_configure()'s DMIC_STATE_ACTIVE refusal (ZEPHYR_BASE
 * zephyr/drivers/audio/dmic_mcux.c, ~lines 421-424, issue #2133 round 4d):
 * reconfiguring an ALREADY-ACTIVE capture is refused outright, before any
 * validation or hardware touch, closing two failures pdm_decide_start()
 * alone did not:
 *   (A) configure() -> START -> a SECOND, refused configure() (which used
 *       to force-sleep the hardware and reset clk_mode/channel_map even
 *       though record_data was still 1) -> a THIRD, valid configure() ->
 *       START now NOOPs (record_data never went back to 0) over a block
 *       that was silently put to sleep -- dead capture with no error
 *       anywhere.
 *   (B) A reconfigure mid-session swaps pdata->mem_slab out from under an
 *       in-progress data_buffer; DMIC_TRIGGER_STOP would then free that
 *       buffer into the NEW slab, not the one it was actually allocated
 *       from.
 * Both require the app to call DMIC_TRIGGER_STOP (which clears
 * record_data) before it may configure() again.
 *
 * @param active pdata->record_data != 0 -- a capture session is already
 *               running.
 * @return true if configure() may proceed; false if it must return -EBUSY
 *         without touching any state.
 */
static inline bool pdm_configure_allowed(bool active)
{
	return !active;
}

#endif /* ZEPHYR_DRIVERS_AUDIO_ALIF_PDM_TRIGGER_STATE_H_ */
