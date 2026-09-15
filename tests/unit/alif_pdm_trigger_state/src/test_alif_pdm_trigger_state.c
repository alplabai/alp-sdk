/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #2133 round 4c: dmic_alif_pdm_trigger()'s DMIC_TRIGGER_START used to
 * clear `overrun` itself without freeing the in-progress block or draining
 * the queue, and never refused an unconfigured or already-active device --
 * mirroring dmic_mcux_trigger()'s DMIC_TRIGGER_START precedent (ZEPHYR_BASE
 * zephyr/drivers/audio/dmic_mcux.c:606-616) fixes both. Exercised here
 * against the exact refuse/no-op/proceed decision the trigger handler now
 * calls, on the host, with no MMIO/k_msgq/devicetree involved.
 */
#include <stdbool.h>

#include <zephyr/ztest.h>

#include "alif_pdm_trigger_state.h"

ZTEST_SUITE(alif_pdm_trigger_state, NULL, NULL, NULL, NULL, NULL);

/* Never configured (pdata->clk_mode still PDM_MODE_MICROPHONE_SLEEP, or
 * reset there by a refused configure()) -- refuse regardless of the other
 * two flags.
 */
ZTEST(alif_pdm_trigger_state, test_unconfigured_refuses)
{
	zassert_equal(pdm_decide_start(false, false, false), PDM_START_REFUSE);
	zassert_equal(pdm_decide_start(false, true, false), PDM_START_REFUSE);
	zassert_equal(pdm_decide_start(false, false, true), PDM_START_REFUSE);
}

/* A reported, not-yet-cleared drop refuses even though the device IS
 * configured -- this is the case round 4c's fix exists for: START must
 * force a STOP first rather than silently absorb the drop itself.
 */
ZTEST(alif_pdm_trigger_state, test_overrun_refuses_even_when_configured)
{
	zassert_equal(pdm_decide_start(true, true, false), PDM_START_REFUSE);
	/* Still refuses even if (implausibly) also marked active -- overrun
	 * is checked first, unconditionally. */
	zassert_equal(pdm_decide_start(true, true, true), PDM_START_REFUSE);
}

/* Configured, no drop, not yet running -- the normal path: proceed to arm
 * the clock mode and interrupts.
 */
ZTEST(alif_pdm_trigger_state, test_configured_clean_proceeds)
{
	zassert_equal(pdm_decide_start(true, false, false), PDM_START_PROCEED);
}

/* Configured, no drop, already running -- matches dmic_mcux_trigger()'s
 * already-ACTIVE no-op (dmic_mcux.c:610): don't double-arm interrupts or
 * restart bookkeeping over in-flight state.
 */
ZTEST(alif_pdm_trigger_state, test_already_active_is_noop)
{
	zassert_equal(pdm_decide_start(true, false, true), PDM_START_NOOP);
}
