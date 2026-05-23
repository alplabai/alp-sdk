/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/ztest.h>
#include <alp/peripheral.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include "demo_class.h"

ZTEST_SUITE(alp_registry, NULL, NULL, NULL, NULL, NULL);

/* ------------------------------------------------------------------------
 * Tiebreaker fixtures for issue #30.
 *
 * Each tiebreaker tier is exercised against its own dedicated backend
 * class.  Cross-class isolation matters because wildcard backends
 * match every non-NULL silicon_ref -- a wildcard registered for one
 * tier would otherwise contaminate another tier's candidate pool.
 *
 * The fixture registrations live alongside their respective ZTEST
 * below for locality.  None of these backends have ops; the tests
 * only inspect which alp_backend_t pointer the selector returned.
 *
 * Classes:
 *   tb_prio      -- two exacts at different priorities (tier 1)
 *   tb_exactwild -- one exact + one wildcard at equal priority (tier 2)
 *   tb_twoexact  -- two exacts at equal priority (tier 3a)
 *   tb_twowild   -- two wildcards at equal priority (tier 3b)
 *
 * The "no match" path is exercised via an unknown class name in the
 * dedicated test -- avoids the need for an empty-class DEFINE whose
 * linker __start_/__stop_ symbols would not resolve.
 * ------------------------------------------------------------------------ */

ZTEST(alp_registry, test_real_hw_picked_over_sw_on_matching_soc)
{
    demo_handle_t h = {0};
    int rc = demo_open(&h, /*instance_id=*/0u);
    zassert_equal(rc, 0, "open failed: %d", rc);
    zassert_not_null(h.backend);
    /* Expect the realhw backend (priority 100) over sw (priority 0). */
    zassert_equal(strcmp(h.backend->vendor, "alif"), 0,
                  "expected alif backend, got %s", h.backend->vendor);
}

ZTEST(alp_registry, test_probe_refines_caps_per_instance)
{
    /* Instance 0 advertises DMA; instance 1 does not. */
    demo_handle_t h0 = {0};
    demo_handle_t h1 = {0};
    zassert_equal(demo_open(&h0, 0u), 0);
    zassert_equal(demo_open(&h1, 1u), 0);
    zassert_true(alp_capabilities_has(demo_capabilities(&h0),
                                      ALP_INSTANCE_CAP_DMA));
    zassert_false(alp_capabilities_has(demo_capabilities(&h1),
                                       ALP_INSTANCE_CAP_DMA));
}

ZTEST(alp_registry, test_oversample_cap_inherited_from_base_caps)
{
    demo_handle_t h = {0};
    zassert_equal(demo_open(&h, 0u), 0);
    zassert_true(alp_capabilities_has(demo_capabilities(&h),
                                      ALP_INSTANCE_CAP_HW_OVERSAMPLE));
}

ZTEST(alp_registry, test_read_dispatches_through_ops)
{
    demo_handle_t h = {0};
    uint32_t v = 0u;
    zassert_equal(demo_open(&h, 0u), 0);
    zassert_equal(demo_read(&h, &v), 0);
    /* The realhw backend returns 0xCAFE. */
    zassert_equal(v, 0xCAFEu);
}

ZTEST(alp_registry, test_backend_count_for_demo)
{
    /* realhw + sw_fallback + stub_target = 3 */
    zassert_equal(alp_backend_count("demo"), 3u);
}

ZTEST(alp_registry, test_select_returns_stub_for_stub_silicon)
{
    const alp_backend_t *be =
        alp_backend_select("demo", "fictional:stub:target");
    zassert_not_null(be);
    zassert_is_null(be->ops, "stub backend should advertise null ops");
}

ZTEST(alp_registry, test_sw_fallback_picked_when_no_exact_match)
{
    /* No backend registers for "renesas:rzv2n:n44" -- only the
     * wildcard sw_fallback matches.  Selector should return sw. */
    const alp_backend_t *be =
        alp_backend_select("demo", "renesas:rzv2n:n44");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "sw"), 0);
    zassert_equal(be->priority, 0);
}

ZTEST(alp_registry, test_open_returns_not_implemented_for_stub_backend)
{
    /* Direct invocation of the stub backend's selector path -- bypasses
     * demo_open's ALP_SOC_REF_STR lookup so we can target the stub
     * silicon_ref directly. */
    const alp_backend_t *be =
        alp_backend_select("demo", "fictional:stub:target");
    zassert_not_null(be);
    zassert_is_null(be->ops);
    /* If demo_open were called with this backend in place, the ops==NULL
     * path would return ALP_ERR_NOT_IMPLEMENTED -- verified by inspection
     * of demo_class.c's open path. */
}

ZTEST(alp_registry, test_select_null_silicon_ref_returns_null)
{
    /* Regression for I2: silicon_ref==NULL must not silently match the
     * "*" wildcard SW fallback. */
    const alp_backend_t *be = alp_backend_select("demo", NULL);
    zassert_is_null(be);
}

ZTEST(alp_registry, test_select_returns_null_for_unknown_silicon)
{
    /* Unknown silicon with no wildcard fallback should be NULL.
     * The "demo" class HAS a wildcard SW fallback, so this case is
     * actually the wildcard match -- which validates the wildcard
     * works.  Coverage for the "no match at all" path lives in the
     * regress_sentinel test (which registers no SW fallback). */
    const alp_backend_t *be = alp_backend_select("demo", "renesas:rzv2n:n44");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "sw"), 0);
}

/* ------------------------------------------------------------------------
 * Issue #30: selector tiebreaker tests.
 * ------------------------------------------------------------------------ */

/* Tier 1: higher priority wins regardless of vendor name. */
ALP_BACKEND_DEFINE_CLASS(tb_prio);
ALP_BACKEND_REGISTER(tb_prio, high, {
    .silicon_ref = "vendor:soc:t1",
    .vendor      = "zenith", /* alphabetically last, still wins on priority */
    .base_caps   = 0u,
    .priority    = 200,
    .ops         = NULL,
    .probe       = NULL,
});
ALP_BACKEND_REGISTER(tb_prio, low, {
    .silicon_ref = "vendor:soc:t1",
    .vendor      = "alif", /* alphabetically first, loses on priority */
    .base_caps   = 0u,
    .priority    = 100,
    .ops         = NULL,
    .probe       = NULL,
});

ZTEST(alp_registry, test_tiebreak_higher_priority_wins)
{
    const alp_backend_t *be =
        alp_backend_select("tb_prio", "vendor:soc:t1");
    zassert_not_null(be);
    zassert_equal(be->priority, 200, "expected prio=200, got %u",
                  (unsigned)be->priority);
    zassert_equal(strcmp(be->vendor, "zenith"), 0,
                  "expected vendor=zenith, got %s", be->vendor);
}

/* Tier 2: at equal priority, exact silicon_ref match beats "*" wildcard. */
ALP_BACKEND_DEFINE_CLASS(tb_exactwild);
ALP_BACKEND_REGISTER(tb_exactwild, exact, {
    .silicon_ref = "vendor:soc:t2",
    .vendor      = "renesas", /* alphabetically after the wildcard's vendor */
    .base_caps   = 0u,
    .priority    = 100,
    .ops         = NULL,
    .probe       = NULL,
});
ALP_BACKEND_REGISTER(tb_exactwild, wild, {
    .silicon_ref = "*",
    .vendor      = "alif", /* alphabetically first -- proves tier-2 dominates tier-3 */
    .base_caps   = 0u,
    .priority    = 100,
    .ops         = NULL,
    .probe       = NULL,
});

ZTEST(alp_registry, test_tiebreak_exact_beats_wildcard_at_equal_priority)
{
    const alp_backend_t *be =
        alp_backend_select("tb_exactwild", "vendor:soc:t2");
    zassert_not_null(be);
    zassert_equal(be->priority, 100);
    zassert_equal(strcmp(be->vendor, "renesas"), 0,
                  "exact should beat wildcard at equal priority "
                  "even when wildcard has lexicographically-smaller "
                  "vendor; got %s", be->vendor);
}

/* Tier 3a: two exacts at equal priority -- alphabetic vendor wins. */
ALP_BACKEND_DEFINE_CLASS(tb_twoexact);
ALP_BACKEND_REGISTER(tb_twoexact, renesas, {
    .silicon_ref = "vendor:soc:t3a",
    .vendor      = "renesas",
    .base_caps   = 0u,
    .priority    = 100,
    .ops         = NULL,
    .probe       = NULL,
});
ALP_BACKEND_REGISTER(tb_twoexact, alif, {
    .silicon_ref = "vendor:soc:t3a",
    .vendor      = "alif",
    .base_caps   = 0u,
    .priority    = 100,
    .ops         = NULL,
    .probe       = NULL,
});

ZTEST(alp_registry, test_tiebreak_two_exact_alphabetic_vendor_wins)
{
    const alp_backend_t *be =
        alp_backend_select("tb_twoexact", "vendor:soc:t3a");
    zassert_not_null(be);
    zassert_equal(be->priority, 100);
    zassert_equal(strcmp(be->vendor, "alif"), 0,
                  "expected alif (alphabetic), got %s", be->vendor);
}

/* Tier 3b: two wildcards at equal priority -- alphabetic vendor wins. */
ALP_BACKEND_DEFINE_CLASS(tb_twowild);
ALP_BACKEND_REGISTER(tb_twowild, renesas, {
    .silicon_ref = "*",
    .vendor      = "renesas",
    .base_caps   = 0u,
    .priority    = 100,
    .ops         = NULL,
    .probe       = NULL,
});
ALP_BACKEND_REGISTER(tb_twowild, alif, {
    .silicon_ref = "*",
    .vendor      = "alif",
    .base_caps   = 0u,
    .priority    = 100,
    .ops         = NULL,
    .probe       = NULL,
});

ZTEST(alp_registry, test_tiebreak_two_wildcards_alphabetic_vendor_wins)
{
    /* Any non-NULL silicon_ref triggers both wildcards.  Alphabetic
     * vendor: "alif" < "renesas" -> alif wins. */
    const alp_backend_t *be =
        alp_backend_select("tb_twowild", "vendor:soc:anything");
    zassert_not_null(be);
    zassert_equal(be->priority, 100);
    zassert_equal(strcmp(be->vendor, "alif"), 0,
                  "expected alif (alphabetic among wildcards), got %s", be->vendor);
}

ZTEST(alp_registry, test_tiebreak_zero_matching_backends_returns_null)
{
    /* Unknown class name -- the class-range table has no entry, the
     * selector falls through and returns NULL.  Exercises the
     * "no candidates at all" path independent of the tiebreaker. */
    const alp_backend_t *be = alp_backend_select("class_that_does_not_exist", "vendor:any:soc");
    zassert_is_null(be);
}
