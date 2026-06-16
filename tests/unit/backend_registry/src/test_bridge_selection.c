/* SPDX-License-Identifier: Apache-2.0
 *
 * Bridge-selection tests (closes #32).
 *
 * The alp-sdk ships three GD32 supervisor bridge backends today
 * (src/backends/{adc,counter,qenc}/gd32_bridge.c) that register
 * with silicon_ref == "renesas:rzv2n:n44" at a HIGH priority so
 * they win selection on V2N silicon, even when a generic Zephyr
 * driver-class backend OR a wildcard SW fallback also registers.
 *
 * These tests mirror that registration shape in dedicated
 * test-only classes (tb_bridge_adc, tb_bridge_counter,
 * tb_bridge_qenc) to confirm the selector actually fires the
 * way the bridge contract expects:
 *
 *   - on V2N silicon       -> gd32_bridge (exact match, prio 100)
 *   - on Alif Ensemble E7  -> zephyr_drv  (wildcard,   prio 50)
 *   - on NULL silicon_ref  -> NULL        (per current contract)
 *
 * The ops vtables are intentionally NULL: these tests only
 * exercise the selector, not ops dispatch.  Real bridge ops are
 * already covered by their per-class peripheral tests.
 *
 * Lives in a separate file so PR #64's selector tiebreaker work
 * (which touches test_registry.c) can merge independently.
 */

#include <string.h>
#include <zephyr/ztest.h>
#include <alp/backend.h>

/* ------------------------------------------------------------------
 * Test-only classes -- one per real bridge class.
 * ------------------------------------------------------------------ */

ALP_BACKEND_DEFINE_CLASS(tb_bridge_adc);
ALP_BACKEND_DEFINE_CLASS(tb_bridge_counter);
ALP_BACKEND_DEFINE_CLASS(tb_bridge_qenc);

/* ------------------------------------------------------------------
 * Per-class fake backend trios.
 *
 * Each class gets three registrations:
 *   - sw_fallback: wildcard, prio 0   -- universal floor
 *   - zephyr_drv:  wildcard, prio 50  -- generic real backend
 *   - gd32_bridge: V2N exact, prio 100 -- bridge under test
 *
 * Ops are NULL throughout: only selection is exercised here.
 * ------------------------------------------------------------------ */

/* ---- ADC class -------------------------------------------------- */
ALP_BACKEND_REGISTER(tb_bridge_adc,
                     sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = NULL,
                         .probe       = NULL,
                     });

ALP_BACKEND_REGISTER(tb_bridge_adc,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr_drv",
                         .base_caps   = 0u,
                         .priority    = 50,
                         .ops         = NULL,
                         .probe       = NULL,
                     });

ALP_BACKEND_REGISTER(tb_bridge_adc,
                     gd32_bridge,
                     {
                         .silicon_ref = "renesas:rzv2n:n44",
                         .vendor      = "gd32_bridge",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = NULL,
                         .probe       = NULL,
                     });

/* ---- Counter class ---------------------------------------------- */
ALP_BACKEND_REGISTER(tb_bridge_counter,
                     sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = NULL,
                         .probe       = NULL,
                     });

ALP_BACKEND_REGISTER(tb_bridge_counter,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr_drv",
                         .base_caps   = 0u,
                         .priority    = 50,
                         .ops         = NULL,
                         .probe       = NULL,
                     });

ALP_BACKEND_REGISTER(tb_bridge_counter,
                     gd32_bridge,
                     {
                         .silicon_ref = "renesas:rzv2n:n44",
                         .vendor      = "gd32_bridge",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = NULL,
                         .probe       = NULL,
                     });

/* ---- QEnc class ------------------------------------------------- */
ALP_BACKEND_REGISTER(tb_bridge_qenc,
                     sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = NULL,
                         .probe       = NULL,
                     });

ALP_BACKEND_REGISTER(tb_bridge_qenc,
                     zephyr_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "zephyr_drv",
                         .base_caps   = 0u,
                         .priority    = 50,
                         .ops         = NULL,
                         .probe       = NULL,
                     });

ALP_BACKEND_REGISTER(tb_bridge_qenc,
                     gd32_bridge,
                     {
                         .silicon_ref = "renesas:rzv2n:n44",
                         .vendor      = "gd32_bridge",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = NULL,
                         .probe       = NULL,
                     });

/* ------------------------------------------------------------------
 * Helper: assert the selector returns a backend whose vendor matches.
 * ------------------------------------------------------------------ */
static void
assert_vendor(const char *class_name, const char *silicon_ref, const char *expected_vendor)
{
	const alp_backend_t *be = alp_backend_select(class_name, silicon_ref);
	zassert_not_null(be, "no backend for class=%s silicon=%s", class_name, silicon_ref);
	zassert_equal(strcmp(be->vendor, expected_vendor),
	              0,
	              "class=%s silicon=%s: expected vendor=%s, got %s",
	              class_name,
	              silicon_ref,
	              expected_vendor,
	              be->vendor);
}

/* ------------------------------------------------------------------
 * Tests -- one suite, three triples (one per class).
 * ------------------------------------------------------------------ */
ZTEST_SUITE(alp_bridge_selection, NULL, NULL, NULL, NULL, NULL);

/* ---- ADC -------------------------------------------------------- */
ZTEST(alp_bridge_selection, test_adc_v2n_picks_gd32_bridge)
{
	/* On V2N: bridge has exact silicon match + prio 100 -> wins. */
	assert_vendor("tb_bridge_adc", "renesas:rzv2n:n44", "gd32_bridge");
}

ZTEST(alp_bridge_selection, test_adc_alif_picks_zephyr_drv)
{
	/* On Alif: bridge doesn't match silicon; zephyr_drv (prio 50)
     * beats sw_fallback (prio 0) among the wildcard candidates. */
	assert_vendor("tb_bridge_adc", "alif:ensemble:e7", "zephyr_drv");
}

ZTEST(alp_bridge_selection, test_adc_null_silicon_returns_null)
{
	/* Per current contract: NULL silicon_ref -> no match. */
	const alp_backend_t *be = alp_backend_select("tb_bridge_adc", NULL);
	zassert_is_null(be);
}

/* ---- Counter ---------------------------------------------------- */
ZTEST(alp_bridge_selection, test_counter_v2n_picks_gd32_bridge)
{
	assert_vendor("tb_bridge_counter", "renesas:rzv2n:n44", "gd32_bridge");
}

ZTEST(alp_bridge_selection, test_counter_alif_picks_zephyr_drv)
{
	assert_vendor("tb_bridge_counter", "alif:ensemble:e7", "zephyr_drv");
}

ZTEST(alp_bridge_selection, test_counter_null_silicon_returns_null)
{
	const alp_backend_t *be = alp_backend_select("tb_bridge_counter", NULL);
	zassert_is_null(be);
}

/* ---- QEnc ------------------------------------------------------- */
ZTEST(alp_bridge_selection, test_qenc_v2n_picks_gd32_bridge)
{
	assert_vendor("tb_bridge_qenc", "renesas:rzv2n:n44", "gd32_bridge");
}

ZTEST(alp_bridge_selection, test_qenc_alif_picks_zephyr_drv)
{
	assert_vendor("tb_bridge_qenc", "alif:ensemble:e7", "zephyr_drv");
}

ZTEST(alp_bridge_selection, test_qenc_null_silicon_returns_null)
{
	const alp_backend_t *be = alp_backend_select("tb_bridge_qenc", NULL);
	zassert_is_null(be);
}
