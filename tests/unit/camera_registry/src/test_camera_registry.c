/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the Camera registry dispatcher.  Mirrors the
 * Slice 1 adc_registry / Slice 4a rtc_registry harnesses.  No
 * vendor extensions exist for camera in Slice 5, so the test
 * surface is the bare selector + capability-getter + public-API
 * edges.
 *
 * Backends visible on this test build:
 *   zephyr_stub     (priority 0, "*" wildcard, vendor "stub")
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("camera", ALP_SOC_REF_STR)`
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
#include <alp/camera.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "../../../../src/backends/camera/camera_ops.h"

ZTEST_SUITE(alp_camera_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_camera_registry, test_stub_picked_for_alif_e7)
{
    /* The wildcard stub is the only camera backend registered in
     * Slice 5; any silicon_ref resolves to it. */
    const alp_backend_t *be =
        alp_backend_select("camera", "alif:ensemble:e7");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "stub"), 0);
    zassert_equal(be->priority, 0);
}

ZTEST(alp_camera_registry, test_stub_picked_for_unknown_silicon)
{
    const alp_backend_t *be =
        alp_backend_select("camera", "fictional:soc:zz");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "stub"), 0);
    zassert_equal(be->priority, 0);
}

ZTEST(alp_camera_registry, test_select_returns_null_for_null_class)
{
    zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_camera_registry, test_select_returns_null_for_null_silicon_ref)
{
    /* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
    zassert_is_null(alp_backend_select("camera", NULL));
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_camera_registry, test_camera_open_returns_null_on_null_cfg)
{
    /* Dispatcher rejects NULL config at the front door before any
     * backend gets called. */
    alp_camera_t *h = alp_camera_open(NULL);
    zassert_is_null(h);
}

ZTEST(alp_camera_registry, test_camera_capabilities_returns_null_for_null_handle)
{
    zassert_is_null(alp_camera_capabilities(NULL));
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_camera_registry, test_backend_count_for_camera)
{
    /* Only zephyr_stub registered on this build -- no vendor-specific
     * camera backends exist in Slice 5. */
    zassert_equal(alp_backend_count("camera"), 1u);
}
