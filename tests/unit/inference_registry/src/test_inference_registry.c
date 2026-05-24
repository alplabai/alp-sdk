/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the inference registry dispatcher.
 *
 * Backends visible on this test build:
 *   sw_fallback     (priority 0,   "*" wildcard, vendor "sw_fallback")
 *
 * Real bodies (tflm, ethos_u_aen, ethos_u_n93, drpai_v2n stub,
 * deepx_dxm1 stub) do NOT link into this native_sim test build --
 * they all depend on either TFLM (not on native_sim) or vendor
 * silicon-specific Kconfigs.  The dispatcher, vendor-ext gating
 * (renesas / deepx surfaces), and selector code paths are
 * exercised against the SW fallback + fabricated handles pinned
 * to specific vendor strings.
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("inference", ALP_SOC_REF_STR)`
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
#include <alp/ext/deepx/inference.h>
#include <alp/ext/renesas/inference.h>
#include <alp/inference.h>
#include <alp/peripheral.h>

#include "../../../../src/backends/inference/inference_ops.h"

ZTEST_SUITE(alp_inference_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_inference_registry, test_sw_fallback_picked_for_alif_e7)
{
    /* sw_fallback is the only inference backend linked on this
     * test build.  Any silicon_ref resolves to it. */
    const alp_backend_t *be =
        alp_backend_select("inference", "alif:ensemble:e7");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "sw_fallback"), 0);
    zassert_equal(be->priority, 0);
}

ZTEST(alp_inference_registry, test_sw_fallback_picked_for_unknown_silicon)
{
    const alp_backend_t *be =
        alp_backend_select("inference", "fictional:soc:zz");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "sw_fallback"), 0);
}

ZTEST(alp_inference_registry, test_select_returns_null_for_null_class)
{
    zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_inference_registry, test_select_returns_null_for_null_silicon_ref)
{
    /* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
    zassert_is_null(alp_backend_select("inference", NULL));
}

ZTEST(alp_inference_registry, test_backend_count_includes_sw_fallback)
{
    /* sw_fallback is the only inference backend in this build;
     * count must be at least 1. */
    zassert_true(alp_backend_count("inference") >= 1u);
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_inference_registry, test_open_returns_null_on_null_cfg)
{
    alp_inference_t *inf = alp_inference_open(NULL);
    zassert_is_null(inf);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_inference_registry, test_open_returns_null_on_zero_model_size)
{
    uint8_t dummy_model[4] = {0};
    alp_inference_config_t cfg = {
        .model_data  = dummy_model,
        .model_size  = 0u,
        .format      = ALP_INFERENCE_MODEL_TFLITE,
        .backend     = ALP_INFERENCE_BACKEND_AUTO,
        .arena_bytes = 0u,
        .arena       = NULL,
    };
    alp_inference_t *inf = alp_inference_open(&cfg);
    zassert_is_null(inf);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_inference_registry, test_open_returns_null_on_null_model_data)
{
    alp_inference_config_t cfg = {
        .model_data  = NULL,
        .model_size  = 16u,
        .format      = ALP_INFERENCE_MODEL_TFLITE,
        .backend     = ALP_INFERENCE_BACKEND_AUTO,
        .arena_bytes = 0u,
        .arena       = NULL,
    };
    alp_inference_t *inf = alp_inference_open(&cfg);
    zassert_is_null(inf);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_inference_registry, test_open_via_sw_fallback_returns_nosupport)
{
    /* sw_fallback is a NOSUPPORT stub on native_sim (no inference
     * engine linked): open reports unsupported and the dispatcher
     * relays it as a NULL handle + last_error = NOSUPPORT.  Mirrors
     * the inference.smoke zephyr contract. */
    uint8_t dummy_model[16] = {0};
    alp_inference_config_t cfg = {
        .model_data  = dummy_model,
        .model_size  = sizeof(dummy_model),
        .format      = ALP_INFERENCE_MODEL_TFLITE,
        .backend     = ALP_INFERENCE_BACKEND_AUTO,
        .arena_bytes = 0u,
        .arena       = NULL,
    };
    zassert_is_null(alp_inference_open(&cfg));
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_inference_registry, test_capabilities_returns_null_for_null_handle)
{
    zassert_is_null(alp_inference_capabilities(NULL));
}

ZTEST(alp_inference_registry, test_close_on_null_is_noop)
{
    /* Close on NULL must not crash; mirrors the camera + storage
     * dispatcher contract. */
    alp_inference_close(NULL);
}

ZTEST(alp_inference_registry, test_invoke_on_null_returns_not_ready)
{
    zassert_equal(alp_inference_invoke(NULL), ALP_ERR_NOT_READY);
}

ZTEST(alp_inference_registry, test_get_input_on_null_returns_not_ready)
{
    alp_inference_tensor_t t = {0};
    zassert_equal(alp_inference_get_input(NULL, 0u, &t), ALP_ERR_NOT_READY);
}

/* The former open-success tests -- handle-based NULL-out INVAL and
 * pool exhaustion -- are removed: sw_fallback's open is a NOSUPPORT
 * stub on native_sim, so no live handle is ever returned.  NULL-handle
 * arg validation is covered by test_{invoke,get_input}_on_null above. */

/* ---------- Vendor-ext gate tests (Renesas) ------------------------- */

ZTEST(alp_inference_registry, test_renesas_ext_null_handle_returns_inval)
{
    uint32_t status = 0u;
    zassert_equal(
        alp_renesas_inference_pipeline_stage_pin(
            NULL, 0u, ALP_RENESAS_INFERENCE_STAGE_DRP),
        ALP_ERR_INVAL);
    zassert_equal(alp_renesas_inference_ai_sram_reserve(NULL, 1024u),
                  ALP_ERR_INVAL);
    zassert_equal(alp_renesas_inference_get_status(NULL, &status),
                  ALP_ERR_INVAL);
}

ZTEST(alp_inference_registry, test_renesas_ext_non_renesas_returns_not_present)
{
    /* Fabricate a handle pinned to a non-Renesas backend.  Using
     * the in_use flag + a synthesised alp_backend_t lets the test
     * exercise the vendor-string gate without needing a live
     * Renesas SoC in the build. */
    const alp_backend_t fake_be = {
        .silicon_ref = "alif:ensemble:e7",
        .vendor      = "alif",
        .base_caps   = 0u,
        .priority    = 0,
        .ops         = NULL,
        .probe       = NULL,
    };
    struct alp_inference fake = {
        .state       = {0},
        .backend     = &fake_be,
        .cached_caps = {0},
        .in_use      = true,
    };
    uint32_t status = 0u;
    zassert_equal(
        alp_renesas_inference_pipeline_stage_pin(
            &fake, 0u, ALP_RENESAS_INFERENCE_STAGE_DRP),
        ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
    zassert_equal(alp_renesas_inference_ai_sram_reserve(&fake, 1024u),
                  ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
    zassert_equal(alp_renesas_inference_get_status(&fake, &status),
                  ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
}

ZTEST(alp_inference_registry, test_renesas_ext_get_status_inval_on_null_status)
{
    const alp_backend_t fake_be = {
        .silicon_ref = "renesas:rzv2n:n44",
        .vendor      = "renesas",
        .base_caps   = 0u,
        .priority    = 0,
        .ops         = NULL,
        .probe       = NULL,
    };
    struct alp_inference fake = {
        .state       = {0},
        .backend     = &fake_be,
        .cached_caps = {0},
        .in_use      = true,
    };
    zassert_equal(alp_renesas_inference_get_status(&fake, NULL),
                  ALP_ERR_INVAL);
}

ZTEST(alp_inference_registry, test_renesas_ext_invalid_stage_returns_inval)
{
    const alp_backend_t fake_be = {
        .silicon_ref = "renesas:rzv2n:n44",
        .vendor      = "renesas",
        .base_caps   = 0u,
        .priority    = 0,
        .ops         = NULL,
        .probe       = NULL,
    };
    struct alp_inference fake = {
        .state       = {0},
        .backend     = &fake_be,
        .cached_caps = {0},
        .in_use      = true,
    };
    /* Stage value past the documented enum tail. */
    zassert_equal(
        alp_renesas_inference_pipeline_stage_pin(
            &fake, 0u, (alp_renesas_inference_stage_t)99u),
        ALP_ERR_INVAL);
}

ZTEST(alp_inference_registry, test_renesas_ext_zero_reserve_returns_oor)
{
    const alp_backend_t fake_be = {
        .silicon_ref = "renesas:rzv2n:n44",
        .vendor      = "renesas",
        .base_caps   = 0u,
        .priority    = 0,
        .ops         = NULL,
        .probe       = NULL,
    };
    struct alp_inference fake = {
        .state       = {0},
        .backend     = &fake_be,
        .cached_caps = {0},
        .in_use      = true,
    };
    zassert_equal(alp_renesas_inference_ai_sram_reserve(&fake, 0u),
                  ALP_ERR_OUT_OF_RANGE);
    /* > AI-SRAM physical size (1.5 MB) -> OUT_OF_RANGE. */
    zassert_equal(
        alp_renesas_inference_ai_sram_reserve(&fake, 4u * 1024u * 1024u),
        ALP_ERR_OUT_OF_RANGE);
}

/* ---------- Vendor-ext gate tests (DEEPX) --------------------------- */

ZTEST(alp_inference_registry, test_deepx_ext_null_handle_returns_inval)
{
    uint32_t status = 0u;
    zassert_equal(
        alp_deepx_inference_slot_pin(NULL, ALP_DEEPX_INFERENCE_SLOT_0),
        ALP_ERR_INVAL);
    zassert_equal(alp_deepx_inference_dram_tile_reserve(NULL, 1024u),
                  ALP_ERR_INVAL);
    zassert_equal(alp_deepx_inference_get_status(NULL, &status),
                  ALP_ERR_INVAL);
}

ZTEST(alp_inference_registry, test_deepx_ext_non_deepx_returns_not_present)
{
    const alp_backend_t fake_be = {
        .silicon_ref = "alif:ensemble:e7",
        .vendor      = "alif",
        .base_caps   = 0u,
        .priority    = 0,
        .ops         = NULL,
        .probe       = NULL,
    };
    struct alp_inference fake = {
        .state       = {0},
        .backend     = &fake_be,
        .cached_caps = {0},
        .in_use      = true,
    };
    uint32_t status = 0u;
    zassert_equal(
        alp_deepx_inference_slot_pin(&fake, ALP_DEEPX_INFERENCE_SLOT_0),
        ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
    zassert_equal(alp_deepx_inference_dram_tile_reserve(&fake, 1024u),
                  ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
    zassert_equal(alp_deepx_inference_get_status(&fake, &status),
                  ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
}

ZTEST(alp_inference_registry, test_deepx_ext_oor_slot_returns_inval)
{
    const alp_backend_t fake_be = {
        .silicon_ref = "deepx:dx:m1",
        .vendor      = "deepx",
        .base_caps   = 0u,
        .priority    = 0,
        .ops         = NULL,
        .probe       = NULL,
    };
    struct alp_inference fake = {
        .state       = {0},
        .backend     = &fake_be,
        .cached_caps = {0},
        .in_use      = true,
    };
    /* Slot value past SLOT_COUNT. */
    zassert_equal(
        alp_deepx_inference_slot_pin(
            &fake, (alp_deepx_inference_slot_t)99u),
        ALP_ERR_INVAL);
}

ZTEST(alp_inference_registry, test_deepx_ext_zero_tile_returns_oor)
{
    const alp_backend_t fake_be = {
        .silicon_ref = "deepx:dx:m1",
        .vendor      = "deepx",
        .base_caps   = 0u,
        .priority    = 0,
        .ops         = NULL,
        .probe       = NULL,
    };
    struct alp_inference fake = {
        .state       = {0},
        .backend     = &fake_be,
        .cached_caps = {0},
        .in_use      = true,
    };
    zassert_equal(alp_deepx_inference_dram_tile_reserve(&fake, 0u),
                  ALP_ERR_OUT_OF_RANGE);
    /* > V2N-M1 DDR carve-out (256 MB) -> OUT_OF_RANGE. */
    zassert_equal(
        alp_deepx_inference_dram_tile_reserve(&fake,
                                              512u * 1024u * 1024u),
        ALP_ERR_OUT_OF_RANGE);
}
