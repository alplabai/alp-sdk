/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the security registry dispatcher.  The security
 * class combines the multi-handle shape (hash + AEAD have handle
 * pools) with the stateless fast-path (alp_random_bytes routes
 * through a cached ops pointer, no handle), so the test surface
 * covers BOTH families of edges.
 *
 * Backends visible on this test build:
 *   zephyr_drv      (priority 100, "*" wildcard)
 *   sw_fallback     (priority 0,   "*" wildcard)
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("security", ALP_SOC_REF_STR)`
 * exercises the same selector code path real customer builds hit.
 * Tests that need a different silicon_ref call alp_backend_select
 * directly.  CONFIG_ALP_SDK_SECURITY stays OFF -- the test only
 * exercises the dispatcher's gates + the selector + the capability
 * getters + the SW-fallback random fill; none of which touch PSA
 * Crypto or MbedTLS.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/security.h>

#include "../../../../src/backends/security/security_ops.h"

ZTEST_SUITE(alp_security_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_security_registry, test_zephyr_drv_picked_over_sw_on_alif_e7)
{
    /* The zephyr_drv (PSA Crypto via MbedTLS) only compiles when
     * CONFIG_MBEDTLS=y.  On native_sim the upstream Zephyr v4.4 /
     * mbedtls 3.6 ssl_misc.h header is broken (see memory note
     * reference_zephyr_ci_gotchas §8), so MBEDTLS is disabled in
     * the test prj.conf.  In that case only sw_fallback is
     * registered and the assertion shape flips. */
    const alp_backend_t *be =
        alp_backend_select("security", "alif:ensemble:e7");
    zassert_not_null(be);
    if (IS_ENABLED(CONFIG_MBEDTLS)) {
        zassert_equal(strcmp(be->vendor, "zephyr"), 0);
        zassert_equal(be->priority, 100);
    } else {
        zassert_equal(strcmp(be->vendor, "sw_fallback"), 0);
    }
}

ZTEST(alp_security_registry, test_sw_fallback_picked_for_unknown_silicon)
{
    /* sw_fallback is always reachable; zephyr_drv joins it when
     * CONFIG_MBEDTLS=y -- on native_sim that's off (see note above)
     * so only sw_fallback registers. */
    const alp_backend_t *be =
        alp_backend_select("security", "fictional:soc:zz");
    zassert_not_null(be);
    (void)be;
    const size_t expected_min = IS_ENABLED(CONFIG_MBEDTLS) ? 2u : 1u;
    zassert_true(alp_backend_count("security") >= expected_min);
}

ZTEST(alp_security_registry, test_select_returns_null_for_null_class)
{
    zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_security_registry, test_select_returns_null_for_null_silicon_ref)
{
    /* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
    zassert_is_null(alp_backend_select("security", NULL));
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_security_registry, test_hash_open_returns_null_on_bad_alg)
{
    /* Out-of-range alg.  On this test build CONFIG_ALP_SDK_SECURITY
     * is OFF so the zephyr_drv body returns NOSUPPORT for every alg
     * (the open() always fails); both the bad-alg validation path
     * and the NOSUPPORT path resolve to a NULL return from the
     * dispatcher, which is the contract this case asserts.  Cast
     * via uint32_t to dodge the -Wenum-compare-conditional warning
     * on the larger sentinel value.
     *
     * When MBEDTLS is OFF the sw_fallback wins selection -- it
     * returns NOSUPPORT for every alg (no real hash implementation),
     * which the dispatcher also relays as NULL, so the assertion
     * holds under both build configurations. */
    alp_hash_alg_t bad = (alp_hash_alg_t)0xFFFFu;
    zassert_is_null(alp_hash_open(bad));
}

ZTEST(alp_security_registry, test_random_bytes_inval_on_null_out)
{
    zassert_equal(alp_random_bytes(NULL, 16), ALP_ERR_INVAL);
}

ZTEST(alp_security_registry, test_hash_capabilities_returns_null_for_null_handle)
{
    zassert_is_null(alp_hash_capabilities(NULL));
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_security_registry, test_backend_count_for_security)
{
    /* sw_fallback always registers.  zephyr_drv joins it when
     * CONFIG_MBEDTLS=y (real-silicon builds via TF-M); on native_sim
     * MBEDTLS is disabled to dodge the upstream ssl_misc.h bug. */
    const size_t expected = IS_ENABLED(CONFIG_MBEDTLS) ? 2u : 1u;
    zassert_equal(alp_backend_count("security"), expected);
}
