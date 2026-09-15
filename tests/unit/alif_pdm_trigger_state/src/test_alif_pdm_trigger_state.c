/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #2133 round 4c: dmic_alif_pdm_trigger()'s DMIC_TRIGGER_START used to
 * clear `overrun` itself without freeing the in-progress block or draining
 * the queue, and never refused an unconfigured or already-active device --
 * mirroring dmic_mcux_trigger()'s DMIC_TRIGGER_START precedent (ZEPHYR_BASE
 * zephyr/drivers/audio/dmic_mcux.c:606-616) fixes both. Round 4d adds
 * dmic_alif_pdm_configure()'s own DMIC_STATE_ACTIVE refusal (same file,
 * ~lines 421-424), closing a reconfigure-while-active hole trigger state
 * alone couldn't. Exercised here against the exact decisions the
 * configure/trigger handlers now call, on the host, with no
 * MMIO/k_msgq/devicetree involved.
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

/* Issue #2133 round 4d: dmic_alif_pdm_configure() never checked whether a
 * capture session was already active, so a reconfigure mid-session could
 * force-sleep the hardware / reset clk_mode out from under a still-running
 * capture (pdm_decide_start()'s STARTs NOOP relied on record_data staying
 * accurate), or swap pdata->mem_slab out from under an in-progress
 * data_buffer. pdm_configure_allowed() mirrors dmic_mcux_configure()'s
 * DMIC_STATE_ACTIVE refusal (ZEPHYR_BASE zephyr/drivers/audio/dmic_mcux.c,
 * ~lines 421-424).
 */
ZTEST(alif_pdm_trigger_state, test_configure_refused_while_active)
{
	zassert_false(pdm_configure_allowed(true));
}

ZTEST(alif_pdm_trigger_state, test_configure_allowed_while_idle)
{
	zassert_true(pdm_configure_allowed(false));
}
