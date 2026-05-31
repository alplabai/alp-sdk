/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Smoke tests for the <alp/inference.h> wrapper under native_sim.
 *
 * The host build doesn't enable any inference backend so the
 * wrapper falls back to ALP_ERR_NOSUPPORT.  Tests verify the
 * dispatcher's NULL-arg / cfg-validation / pool-bounds branches
 * plus the NOSUPPORT contract.  Real TFLM + Ethos-U execution
 * lands in HW-in-loop CI.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"
#include "alp/inference.h"

ZTEST_SUITE(alp_inference, NULL, NULL, NULL, NULL, NULL);

/* A 16-byte placeholder model -- not a real flatbuffer.  All we
 * need is non-NULL/non-zero so the dispatcher reaches the backend
 * selector.  The fall-back returns NOSUPPORT before parsing. */
static const uint8_t k_placeholder_model[16] = {0xDE, 0xAD, 0xBE, 0xEF};

ZTEST(alp_inference, test_open_no_backend_returns_null)
{
    alp_inference_t *h = alp_inference_open(&(alp_inference_config_t){
        .model_data = k_placeholder_model,
        .model_size = sizeof(k_placeholder_model),
        .format     = ALP_INFERENCE_MODEL_TFLITE,
        .backend    = ALP_INFERENCE_BACKEND_AUTO,
    });
    zassert_is_null(h, "alp_inference_open without a backend must yield NULL");
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT, "expected NOSUPPORT, got %d",
                  (int)alp_last_error());
}

ZTEST(alp_inference, test_open_explicit_cpu_returns_null)
{
    alp_inference_t *h = alp_inference_open(&(alp_inference_config_t){
        .model_data = k_placeholder_model,
        .model_size = sizeof(k_placeholder_model),
        .backend    = ALP_INFERENCE_BACKEND_CPU,
    });
    zassert_is_null(h);
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_inference, test_open_explicit_ethos_u_returns_null)
{
    alp_inference_t *h = alp_inference_open(&(alp_inference_config_t){
        .model_data = k_placeholder_model,
        .model_size = sizeof(k_placeholder_model),
        .backend    = ALP_INFERENCE_BACKEND_ETHOS_U,
    });
    zassert_is_null(h);
    zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

ZTEST(alp_inference, test_open_null_cfg_invalid)
{
    alp_inference_t *h = alp_inference_open(NULL);
    zassert_is_null(h);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_inference, test_open_null_model_invalid)
{
    alp_inference_t *h = alp_inference_open(&(alp_inference_config_t){
        .model_data = NULL,
        .model_size = 16,
    });
    zassert_is_null(h);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_inference, test_open_zero_size_invalid)
{
    alp_inference_t *h = alp_inference_open(&(alp_inference_config_t){
        .model_data = k_placeholder_model,
        .model_size = 0,
    });
    zassert_is_null(h);
    zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_inference, test_lifecycle_null_handle_safe)
{
    zassert_equal(alp_inference_num_inputs(NULL), 0u);
    zassert_equal(alp_inference_num_outputs(NULL), 0u);

    alp_inference_tensor_t t;
    zassert_equal(alp_inference_get_input(NULL, 0, &t), ALP_ERR_NOT_READY);
    zassert_equal(alp_inference_get_output(NULL, 0, &t), ALP_ERR_NOT_READY);
    zassert_equal(alp_inference_invoke(NULL), ALP_ERR_NOT_READY);
    alp_inference_close(NULL);
}
