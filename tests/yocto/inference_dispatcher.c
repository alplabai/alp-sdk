/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plain-CMake tests for the Yocto/Linux <alp/inference.h>
 * dispatcher (src/yocto/inference_yocto.c).
 *
 * The Zephyr dispatcher has parallel coverage at
 * tests/zephyr/inference/src/main.c; the two suites share contracts
 * but use different test harnesses (ztest vs the local assert
 * helpers in test_assert.h).
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_inference_dispatcher
 *   ctest --test-dir build -R alp_test_inference_dispatcher
 *
 * No real backend is enabled in this build, so the dispatcher
 * exercises its NULL-arg / cfg-validation / no-backend-available
 * paths.  Backend-specific tests land alongside the v0.4 real
 * DEEPX bring-up.
 */

#include <stdint.h>

#include "alp/inference.h"
#include "alp/peripheral.h"

#include "test_assert.h"

/* A 16-byte placeholder model -- not a real flatbuffer.  Lets the
 * dispatcher reach the backend selector before bailing. */
static const uint8_t k_placeholder_model[16] = {0xDE, 0xAD, 0xBE, 0xEF};

static void          test_null_cfg_returns_null_and_stamps_invalid(void)
{
    alp_inference_t *h = alp_inference_open(NULL);
    ALP_ASSERT_NULL(h);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_null_model_returns_null_and_stamps_invalid(void)
{
    alp_inference_config_t cfg = {
        .model_data = NULL,
        .model_size = 16,
    };
    alp_inference_t *h = alp_inference_open(&cfg);
    ALP_ASSERT_NULL(h);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_zero_size_returns_null_and_stamps_invalid(void)
{
    alp_inference_config_t cfg = {
        .model_data = k_placeholder_model,
        .model_size = 0,
    };
    alp_inference_t *h = alp_inference_open(&cfg);
    ALP_ASSERT_NULL(h);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
}

static void test_auto_with_no_backend_returns_null_and_stamps_nosupport(void)
{
    /* Default build has ALP_SDK_USE_DEEPX_DXM1=OFF, so resolve_auto
     * returns AUTO again and the dispatcher stamps NOSUPPORT. */
    alp_inference_config_t cfg = {
        .model_data = k_placeholder_model,
        .model_size = sizeof(k_placeholder_model),
        .format     = ALP_INFERENCE_MODEL_TFLITE,
        .backend    = ALP_INFERENCE_BACKEND_AUTO,
    };
    alp_inference_t *h = alp_inference_open(&cfg);
    ALP_ASSERT_NULL(h);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_NOSUPPORT);
}

static void test_explicit_unsupported_backend_returns_null(void)
{
    /* CPU / ETHOS_U / DRPAI aren't wired on the Yocto path yet --
     * dispatcher falls through to default NOSUPPORT. */
    alp_inference_config_t cfg = {
        .model_data = k_placeholder_model,
        .model_size = sizeof(k_placeholder_model),
        .backend    = ALP_INFERENCE_BACKEND_CPU,
    };
    alp_inference_t *h = alp_inference_open(&cfg);
    ALP_ASSERT_NULL(h);
    ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_NOSUPPORT);
}

static void test_null_handle_methods_are_safe(void)
{
    /* Every accessor on a NULL handle must short-circuit
     * cleanly -- no crashes, no out-of-band writes. */
    ALP_ASSERT_EQ_INT(alp_inference_num_inputs(NULL), 0u);
    ALP_ASSERT_EQ_INT(alp_inference_num_outputs(NULL), 0u);

    alp_inference_tensor_t t = {0};
    ALP_ASSERT_EQ_INT(alp_inference_get_input(NULL, 0, &t), ALP_ERR_NOT_READY);
    ALP_ASSERT_EQ_INT(alp_inference_get_output(NULL, 0, &t), ALP_ERR_NOT_READY);
    ALP_ASSERT_EQ_INT(alp_inference_invoke(NULL), ALP_ERR_NOT_READY);

    /* alp_inference_close(NULL) must not crash either. */
    alp_inference_close(NULL);
    ALP_TEST_PASS();
}

static void test_get_input_null_out_returns_invalid(void)
{
    /* Even with a NULL handle, the out-NULL check fires first
     * -- the dispatcher returns NOT_READY for closed handles. */
    ALP_ASSERT_EQ_INT(alp_inference_get_input(NULL, 0, NULL), ALP_ERR_NOT_READY);
    ALP_ASSERT_EQ_INT(alp_inference_get_output(NULL, 0, NULL), ALP_ERR_NOT_READY);
}

int main(void)
{
    test_null_cfg_returns_null_and_stamps_invalid();
    test_null_model_returns_null_and_stamps_invalid();
    test_zero_size_returns_null_and_stamps_invalid();
    test_auto_with_no_backend_returns_null_and_stamps_nosupport();
    test_explicit_unsupported_backend_returns_null();
    test_null_handle_methods_are_safe();
    test_get_input_null_out_returns_invalid();

    ALP_TEST_SUMMARY();
}
