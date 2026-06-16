/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the dsp registry dispatcher.  The dsp class carries
 * ONE stateful handle type (alp_dsp_chain_t) on top of the backend
 * registry shipped in Slice 0 (PR #17).
 *
 * Backends visible on this test build:
 *   sw_fallback     (priority 0, "*" wildcard, vendor="sw")
 *
 * DSP is structurally unusual within Slice 4d -- the v0.3 OS-agnostic
 * body became the sw_fallback backend itself rather than splitting
 * into a separate zephyr_drv (no Zephyr driver class backs alp_dsp_*;
 * the V2N HW FFT surfaces via wave-2's <alp/adc.h> composition).
 * The test inventory reflects that: there is exactly ONE backend in
 * the dsp class on this slice.
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("dsp", ALP_SOC_REF_STR)` exercises
 * the same selector code path real customer builds hit.  Cases that
 * need a different silicon_ref call alp_backend_select directly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/dsp.h>
#include <alp/peripheral.h>

#include "../../../../src/backends/dsp/dsp_ops.h"

ZTEST_SUITE(alp_dsp_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_dsp_registry, test_sw_fallback_picked_for_alif_e7)
{
	/* Only sw_fallback is registered against dsp on this slice; the
     * selector must pick it for every silicon_ref including the
     * default alif:ensemble:e7 build pin. */
	const alp_backend_t *be = alp_backend_select("dsp", "alif:ensemble:e7");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "sw_fallback"), 0);
	zassert_equal(be->priority, 0);
}

ZTEST(alp_dsp_registry, test_sw_fallback_picked_for_unknown_silicon)
{
	/* Wildcard backend covers fictional silicon as well -- the
     * standalone DSP path must keep working on any future SoM until a
     * higher-priority HW backend lands. */
	const alp_backend_t *be = alp_backend_select("dsp", "fictional:soc:zz");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "sw_fallback"), 0);
	zassert_equal(be->priority, 0);
}

ZTEST(alp_dsp_registry, test_select_returns_null_for_null_class)
{
	zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_dsp_registry, test_select_returns_null_for_null_silicon_ref)
{
	/* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
	zassert_is_null(alp_backend_select("dsp", NULL));
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_dsp_registry, test_dsp_chain_open_returns_null_on_null_stages)
{
	zassert_is_null(alp_dsp_chain_open(NULL, 1u));
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_dsp_registry, test_dsp_chain_open_returns_null_on_zero_stages)
{
	/* Need a non-NULL stages pointer so the NULL guard does not fire
     * first -- the zero-count branch in the dispatcher must still
     * reject the call. */
	alp_dsp_stage_t stages[1] = { 0 };
	zassert_is_null(alp_dsp_chain_open(stages, 0u));
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_dsp_registry, test_dsp_chain_capabilities_returns_null_for_null_handle)
{
	zassert_is_null(alp_dsp_chain_capabilities(NULL));
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_dsp_registry, test_backend_count_for_dsp)
{
	/* sw_fallback only on Slice 4d.  No vendor-specific backends are
     * registered against dsp yet -- when a V2N standalone-FFT or AEN
     * HW backend lands this assertion is the canary that flags the
     * inventory change. */
	zassert_equal(alp_backend_count("dsp"), 1u);
}
