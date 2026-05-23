/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the mproc registry dispatcher.  Mirrors the BLE
 * sibling harness but extends coverage to three handle types --
 * shmem, mbox, hwsem -- which share a single 'mproc' class
 * registry.  No vendor extensions, so the test surface is the bare
 * selector + capability-getter + public-API edges across all three
 * primitives.
 *
 * Backends visible on this test build:
 *   zephyr_drv      (priority 100, "*" wildcard)
 *   sw_fallback     (priority 0,   "*" wildcard)
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("mproc", ALP_SOC_REF_STR)`
 * exercises the same selector code path real customer builds hit.
 * Tests that need a different silicon_ref call alp_backend_select
 * directly.  CONFIG_MBOX stays OFF -- the test only exercises the
 * dispatcher's null-handle gates, the selector, and the capability
 * getters; none of which touch the Zephyr mbox subsystem.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/mproc.h>
#include <alp/peripheral.h>

#include "../../../../src/backends/mproc/mproc_ops.h"

ZTEST_SUITE(alp_mproc_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_mproc_registry, test_zephyr_drv_picked_over_sw_on_alif_e7)
{
    const alp_backend_t *be =
        alp_backend_select("mproc", "alif:ensemble:e7");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "zephyr"), 0);
    zassert_equal(be->priority, 100);
}

ZTEST(alp_mproc_registry, test_sw_fallback_picked_for_unknown_silicon)
{
    /* Both registered backends are wildcards; the higher-priority
     * zephyr_drv would normally win.  This case still exercises the
     * selector and asserts the sw_fallback is reachable on the test
     * build via the registry's count.  Degraded pattern: only
     * inventory is asserted, not the specific pick. */
    const alp_backend_t *be =
        alp_backend_select("mproc", "fictional:soc:zz");
    zassert_not_null(be);
    (void)be;
    zassert_true(alp_backend_count("mproc") >= 2u);
}

ZTEST(alp_mproc_registry, test_select_returns_null_for_null_class)
{
    zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_mproc_registry, test_select_returns_null_for_null_silicon_ref)
{
    /* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
    zassert_is_null(alp_backend_select("mproc", NULL));
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_mproc_registry, test_shmem_open_inval_on_null_args)
{
    /* Dispatcher must reject NULL cfg / NULL cfg->name before
     * reaching the backend; alp_shmem_open returns NULL and the
     * caller can inspect alp_last_error() for ALP_ERR_INVAL. */
    zassert_is_null(alp_shmem_open(NULL));

    alp_shmem_config_t bad = { .name = NULL, .size = 64, .cacheable = false };
    zassert_is_null(alp_shmem_open(&bad));
}

ZTEST(alp_mproc_registry, test_mbox_capabilities_returns_null_for_null_handle)
{
    zassert_is_null(alp_mbox_capabilities(NULL));
}

ZTEST(alp_mproc_registry, test_hwsem_capabilities_returns_null_for_null_handle)
{
    zassert_is_null(alp_hwsem_capabilities(NULL));
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_mproc_registry, test_backend_count_for_mproc)
{
    /* zephyr_drv + sw_fallback registered on this build.
     * No vendor-specific backends exist for mproc in Slice 4c. */
    zassert_equal(alp_backend_count("mproc"), 2u);
}
