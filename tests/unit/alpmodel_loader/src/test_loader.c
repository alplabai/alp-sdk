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
