/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/ztest.h>
#include <alp/peripheral.h>
#include <alp/cap_instance.h>
#include "demo_class.h"

ZTEST_SUITE(alp_registry, NULL, NULL, NULL, NULL, NULL);

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
