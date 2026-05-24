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
 * The stub is a NOSUPPORT stub: open() reports unsupported (no 2D
 * HAL on this build), so alp_gpu2d_open() returns NULL with
 * last_error = NOSUPPORT; NULL-handle ops surface NOT_READY.
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

ZTEST(alp_gpu2d_registry, test_open_returns_nosupport_no_vendor_hal)
{
    /* The wildcard stub is a NOSUPPORT stub (no 2D HAL on this build),
     * so open reports unsupported and the dispatcher relays it as a
     * NULL handle + last_error = NOSUPPORT. */
    zassert_is_null(alp_gpu2d_open());
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_gpu2d_registry, test_ops_null_handle_not_ready)
{
    /* open() NOSUPPORTs on this build, so callers never hold a live
     * handle; the dispatcher's NULL-handle guard surfaces NOT_READY
     * before any backend op or surface validation.  _valid_src /
     * _valid_dst keep the surface arguments well-formed so NOT_READY
     * is unambiguously the handle check. */
    zassert_equal(alp_gpu2d_fill_rect(NULL, &_valid_dst, 0u, 0u, 4u, 4u, 0xffu), ALP_ERR_NOT_READY);
    zassert_equal(alp_gpu2d_blend(NULL, &_valid_src, 0u, 0u, &_valid_dst, 0u, 0u, 4u, 4u,
                                  (alp_gpu2d_blend_mode_t)0),
                  ALP_ERR_NOT_READY);
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_gpu2d_registry, test_backend_count_for_gpu2d)
{
    /* Only zephyr_stub registered on this build -- no vendor-specific
     * GPU2D backends exist in Slice 8b. */
    zassert_equal(alp_backend_count("gpu2d"), 1u);
}
