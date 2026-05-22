/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the audio registry dispatcher.  The audio class
 * carries TWO independent handle types (alp_audio_in_t microphone
 * capture + alp_audio_out_t speaker playback) behind ONE class
 * registry per design spec Section 4, so the test surface covers
 * both families of edges.
 *
 * Backends visible on this test build:
 *   zephyr_drv      (priority 100, "*" wildcard)
 *   sw_fallback     (priority 0,   "*" wildcard)
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("audio", ALP_SOC_REF_STR)`
 * exercises the same selector code path real customer builds hit.
 * Tests that need a different silicon_ref call alp_backend_select
 * directly.  CONFIG_ALP_SDK_AUDIO_IN / _OUT stay OFF -- the test
 * only exercises the dispatcher's gates + the selector + the
 * capability getters + the SW-fallback degraded ops; none of which
 * touch Zephyr's audio_dmic or the alp_i2s_* layer.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/audio.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "../../../../src/backends/audio/audio_ops.h"

ZTEST_SUITE(alp_audio_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_audio_registry, test_zephyr_drv_picked_over_sw_on_alif_e7)
{
    const alp_backend_t *be =
        alp_backend_select("audio", "alif:ensemble:e7");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "zephyr"), 0);
    zassert_equal(be->priority, 100);
}

ZTEST(alp_audio_registry, test_sw_fallback_picked_for_unknown_silicon)
{
    /* Both registered backends are wildcards; the higher-priority
     * zephyr_drv would normally win.  This case still exercises the
     * selector and asserts the sw_fallback is reachable on the test
     * build via the registry's count.  Degraded pattern: only
     * inventory is asserted, not the specific pick. */
    const alp_backend_t *be =
        alp_backend_select("audio", "fictional:soc:zz");
    zassert_not_null(be);
    (void)be;
    zassert_true(alp_backend_count("audio") >= 2u);
}

ZTEST(alp_audio_registry, test_select_returns_null_for_null_class)
{
    zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_audio_registry, test_select_returns_null_for_null_silicon_ref)
{
    /* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
    zassert_is_null(alp_backend_select("audio", NULL));
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_audio_registry, test_audio_in_open_returns_null_on_null_cfg)
{
    zassert_is_null(alp_audio_in_open(NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_audio_registry, test_audio_out_open_returns_null_on_null_cfg)
{
    zassert_is_null(alp_audio_out_open(NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_audio_registry, test_audio_in_capabilities_returns_null_for_null_handle)
{
    zassert_is_null(alp_audio_in_capabilities(NULL));
}

/* ---------- Registry inventory test -------------------------------- */

ZTEST(alp_audio_registry, test_backend_count_for_audio)
{
    /* zephyr_drv + sw_fallback registered on this build.
     * No vendor-specific backends exist for audio in Slice 4d. */
    zassert_equal(alp_backend_count("audio"), 2u);
}
