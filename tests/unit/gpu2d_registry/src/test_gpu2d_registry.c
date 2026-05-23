/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the GPU2D registry dispatcher.  Mirrors the
 * Slice 5 camera_registry / Slice 7 power_registry / Slice 8a
 * display_registry harnesses.  No vendor extensions exist for
 * GPU2D in Slice 8b, so the test surface is the bare selector +
 * capability-getter + public-API edges.
 *
 * Backends visible on this test build:
 *   zephyr_stub     (priority 0, "*" wildcard, vendor "stub")
 *
 * The stub's open() returns ALP_OK so alp_gpu2d_open() hands back
 * a real handle on this build; the dispatcher's surface-validation
 * INVAL pre-checks are exercised against that real handle.
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("gpu2d", ALP_SOC_REF_STR)`
 * exercises the same selector code path real customer builds hit.
 * Tests that need a different silicon_ref call alp_backend_select
 * directly.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/gpu2d.h>
#include <alp/peripheral.h>

#include "../../../../src/backends/gpu2d/gpu2d_ops.h"

ZTEST_SUITE(alp_gpu2d_registry, NULL, NULL, NULL, NULL, NULL);

/* Scratch backing buffer + valid surfaces for the blend INVAL test.
 * Both src and dst point at the same scratch -- the stub never
 * touches the bytes, the dispatcher only checks pointer + dims +
 * format range. */
static uint8_t _scratch[16];
static const alp_gpu2d_surface_t _valid_src = {
    .base         = _scratch,
    .width        = 4,
    .height       = 4,
    .stride_bytes = 4,
    .format       = ALP_GPU2D_FMT_RGBA8888,
};
static const alp_gpu2d_surface_t _valid_dst = {
    .base         = _scratch,
    .width        = 4,
    .height       = 4,
    .stride_bytes = 4,
    .format       = ALP_GPU2D_FMT_RGBA8888,
};

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_gpu2d_registry, test_stub_picked_for_alif_e7)
{
    /* The wildcard stub is the only GPU2D backend registered in
     * Slice 8b; any silicon_ref resolves to it. */
    const alp_backend_t *be =
        alp_backend_select("gpu2d", "alif:ensemble:e7");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "stub"), 0);
    zassert_equal(be->priority, 0);
}

ZTEST(alp_gpu2d_registry, test_stub_picked_for_unknown_silicon)
{
    const alp_backend_t *be =
        alp_backend_select("gpu2d", "fictional:soc:zz");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "stub"), 0);
    zassert_equal(be->priority, 0);
}

ZTEST(alp_gpu2d_registry, test_select_returns_null_for_null_class)
{
    zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_gpu2d_registry, test_select_returns_null_for_null_silicon_ref)
{
    /* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
    zassert_is_null(alp_backend_select("gpu2d", NULL));
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_gpu2d_registry, test_gpu2d_capabilities_returns_null_for_null_handle)
{
    zassert_is_null(alp_gpu2d_capabilities(NULL));
}

ZTEST(alp_gpu2d_registry, test_fill_rect_inval_on_null_dst)
{
    /* The stub's open() returns ALP_OK so we get a real handle.
     * The dispatcher's surface validation fires before the stub op
     * runs, so a NULL dst surfaces as ALP_ERR_INVAL rather than the
     * stub's ALP_ERR_NOT_IMPLEMENTED. */
    alp_gpu2d_t *h = alp_gpu2d_open();
    zassert_not_null(h);

    zassert_equal(alp_gpu2d_fill_rect(h, NULL, 0u, 0u, 10u, 10u, 0xffu),
                  ALP_ERR_INVAL);

    alp_gpu2d_close(h);
}

ZTEST(alp_gpu2d_registry, test_blend_inval_on_bad_mode)
{
    /* Bad-mode INVAL must fire before the stub op runs.  Both
     * surfaces are valid so the only reason for INVAL is the
     * out-of-range blend mode. */
    alp_gpu2d_t *h = alp_gpu2d_open();
    zassert_not_null(h);

    zassert_equal(alp_gpu2d_blend(h, &_valid_src, 0u, 0u,
                                  &_valid_dst, 0u, 0u, 4u, 4u,
                                  (alp_gpu2d_blend_mode_t)99),
                  ALP_ERR_INVAL);

    alp_gpu2d_close(h);
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_gpu2d_registry, test_backend_count_for_gpu2d)
{
    /* Only zephyr_stub registered on this build -- no vendor-specific
     * GPU2D backends exist in Slice 8b. */
    zassert_equal(alp_backend_count("gpu2d"), 1u);
}
