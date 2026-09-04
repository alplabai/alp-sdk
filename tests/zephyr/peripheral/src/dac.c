/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/peripheral.h> -- DAC wrapper tests.  Extracted from main.c in
 * §C.16.  Covers NULL-cfg INVAL, out-of-range channel, and the
 * NOT_READY-or-NOSUPPORT path when no underlying DAC controller or
 * V2N supervisor is wired.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "alp/backend.h" /* alp_backend_select / alp_backend_t */
#include "alp/dac.h"     /* alp_dac_open / alp_dac_t / alp_dac_config_t */
#include "alp/peripheral.h"
#include "alp/soc_caps.h" /* ALP_SOC_DAC_COUNT / ALP_SOC_REF_STR */

ZTEST(alp_peripheral, test_dac_null_cfg)
{
	zassert_is_null(alp_dac_open(NULL));
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_dac_out_of_range_channel)
{
	/* Channel id 9 is out of range for any E1M part (ALP_E1M_DAC_COUNT = 2).
	 *
	 * Admission is the backend registry, not the SoC cap table (issue
	 * #1642's dispatch-level `ALP_SOC_DAC_COUNT > 0` gate is gone): a
	 * bridged backend on a SoC with ALP_SOC_DAC_COUNT == 0 can still
	 * serve a channel, so alp_dac_open() no longer rejects up front on
	 * the cap table.  Which backend wins (zephyr_drv here, CONFIG_DAC=n
	 * in this suite) decides the code -- NOSUPPORT if the backend
	 * declines outright, INVAL if it bounds-checks the channel itself.
	 * Either way the channel is out of range and open() returns NULL. */
	alp_dac_t *d = alp_dac_open(&(alp_dac_config_t){
	    .channel_id = 9u,
	    .initial_mv = 0u,
	});
	zassert_is_null(d);
}

ZTEST(alp_peripheral, test_dac_unresolved_channel_yields_not_ready)
{
	/* Without a real DAC controller or V2N supervisor backing
     * channel 0, open must fail with NOT_READY (DT-alias path) or
     * NOSUPPORT (CONFIG_DAC=n).  Either is acceptable; both surface
     * as a NULL return. */
	alp_dac_t *d = alp_dac_open(&(alp_dac_config_t){
	    .channel_id = 0u,
	    .initial_mv = 0u,
	});
	zassert_is_null(d);
}

#if defined(CONFIG_ALP_SDK_V2N_SUPERVISOR)
/* Regression pin for issue #1642: on renesas:rzv2n:n44 (this scenario's
 * SoC choice), ALP_SOC_DAC_COUNT == 0 -- the SoC has no on-die DAC -- yet
 * src/backends/dac/gd32_bridge.c registers an EXACT silicon_ref match
 * that serves channels 0/1 over the GD32 bridge.  The deleted dispatch-
 * level `ALP_SOC_DAC_COUNT > 0` gate never actually blocked this case
 * (it was skipped whenever the count was 0), but it stood for the wrong
 * reason and the naive "fix" of flipping the test (reject when count ==
 * 0) would have broken exactly this bridged-channel case.  Prove
 * admission is the registry: a channel gd32_bridge actually serves
 * reaches ITS open() and fails for the real hardware reason (no bus
 * configured -> supervisor NOT_READY), not a cap-table refusal. */
ZTEST(alp_peripheral, test_dac_bridged_channel_admitted_despite_zero_soc_count)
{
	zassert_equal(ALP_SOC_DAC_COUNT, 0, "scenario expects a 0-DAC SoC (renesas:rzv2n:n44)");

	const alp_backend_t *be = alp_backend_select("dac", ALP_SOC_REF_STR);
	zassert_not_null(be, "registry must admit despite ALP_SOC_DAC_COUNT == 0");
	zassert_equal(
	    strcmp(be->vendor, "renesas"), 0, "expected gd32_bridge, got vendor=%s", be->vendor);

	/* channel_id 0 is within gd32_bridge's served range (0/1) even
     * though the SoC cap table says the class has zero instances. */
	alp_dac_t *d = alp_dac_open(&(alp_dac_config_t){
	    .channel_id = 0u,
	    .initial_mv = 0u,
	});
	zassert_is_null(d);
	zassert_equal(alp_last_error(),
	              ALP_ERR_NOT_READY,
	              "channel within the bridge's range must reach gd32_bridge's open() "
	              "(supervisor NOT_READY), not a cap-table refusal");
}
#endif /* CONFIG_ALP_SDK_V2N_SUPERVISOR */
