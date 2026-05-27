/* SPDX-License-Identifier: Apache-2.0
 *
 * Integration test for alp_inference_open_alpmodel().
 *
 * On native_sim no real backend links, so selection picks the cpu("*")
 * target from the fixture and alp_inference_open() returns NOSUPPORT via
 * the sw_fallback stub.  The loader propagates that error: NULL handle +
 * alp_last_error() == ALP_ERR_NOSUPPORT.
 *
 * Pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y + CONFIG_ALP_SDK_MODEL_READER=y
 * so the real parse + select path is exercised.
 */
#include <zephyr/ztest.h>
#include <alp/inference.h>
#include <alp/peripheral.h>
#include "fixture.h" /* alp_model_fixture[], alp_model_fixture_len */

ZTEST_SUITE(alpmodel_loader, NULL, NULL, NULL, NULL, NULL);

ZTEST(alpmodel_loader, test_null_opts_inval)
{
    zassert_is_null(alp_inference_open_alpmodel(NULL));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alpmodel_loader, test_bad_magic_inval)
{
    static const uint8_t  junk[32] = { 'X', 'X', 'X', 'X' };
    alp_model_open_opts_t o        = {
               .data    = junk,
               .size    = sizeof(junk),
               .backend = ALP_INFERENCE_BACKEND_AUTO,
    };
    zassert_is_null(alp_inference_open_alpmodel(&o));
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alpmodel_loader, test_real_fixture_selects_then_delegates_nosupport)
{
    /* minimal.alpmodel has ethos_u(e8) + cpu; on native_sim e7 with no
     * real backend linked, selection picks cpu("*"), open() relays
     * NOSUPPORT. */
    alp_model_open_opts_t o = {
        .data    = alp_model_fixture,
        .size    = alp_model_fixture_len,
        .backend = ALP_INFERENCE_BACKEND_AUTO,
    };
    zassert_is_null(alp_inference_open_alpmodel(&o));
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alpmodel_loader, test_path_only_opts_nosupport)
{
    /* data == NULL (path-based load) is not implemented yet -> NOSUPPORT,
     * NOT INVAL (so a path-only caller can tell it's unsupported, not a bug). */
    alp_model_open_opts_t o = {
        .data    = NULL,
        .path    = "model.alpmodel",
        .backend = ALP_INFERENCE_BACKEND_AUTO,
    };
    zassert_is_null(alp_inference_open_alpmodel(&o));
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alpmodel_loader, test_version_mismatch_returns_version)
{
    /* Well-formed "ALPM" magic but container version 2 (> reader's 1):
     * alp_model_parse returns ALP_ERR_VERSION and the loader propagates it. */
    static const uint8_t  buf[24] = { 'A', 'L', 'P', 'M', 2, 0 }; /* version u16 LE = 2 */
    alp_model_open_opts_t o       = {
              .data    = buf,
              .size    = sizeof(buf),
              .backend = ALP_INFERENCE_BACKEND_AUTO,
    };
    zassert_is_null(alp_inference_open_alpmodel(&o));
    zassert_equal(alp_last_error(), ALP_ERR_VERSION);
}
