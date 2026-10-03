/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Dispatcher-level coverage for alp_deepx_inference_open()
 * (<alp/ext/deepx/inference.h>) in src/yocto/inference_yocto.c.  Same
 * #include-the-.c technique as inference_format_gate.c, with the DEEPX
 * backend forced on and its hooks faked; the fake open() records the core
 * set it was handed so the test can prove it reaches the backend.
 *
 * Covers: NULL cfg and out-of-range cores -> INVAL; a non-DEEPX backend ->
 * INVAL; a non-DXNN format -> INVAL; AUTO is pinned to DEEPX; the core set
 * is forwarded; alp_inference_open() uses the all-cores set (0); a full
 * handle pool -> NOMEM.
 */

#include <stdint.h>

#include "test_assert.h"

#define ALP_SDK_USE_DEEPX_DXM1 1
#include "../../src/yocto/inference_yocto.c"

static int      g_open_calls;
static unsigned g_last_bound = 99u;

alp_status_t
alp_inference_deepx_open(struct alp_inference *h, const alp_inference_config_t *cfg, unsigned bound)
{
	(void)cfg;
	g_open_calls++;
	g_last_bound = bound;
	h->be_state  = NULL;
	return ALP_OK;
}
size_t alp_inference_deepx_num_inputs(struct alp_inference *h)
{
	(void)h;
	return 0u;
}
size_t alp_inference_deepx_num_outputs(struct alp_inference *h)
{
	(void)h;
	return 0u;
}
alp_status_t
alp_inference_deepx_get_input(struct alp_inference *h, size_t index, alp_inference_tensor_t *out)
{
	(void)h;
	(void)index;
	*out = (alp_inference_tensor_t){ 0 };
	return ALP_OK;
}
alp_status_t
alp_inference_deepx_get_output(struct alp_inference *h, size_t index, alp_inference_tensor_t *out)
{
	(void)h;
	(void)index;
	*out = (alp_inference_tensor_t){ 0 };
	return ALP_OK;
}
alp_status_t alp_inference_deepx_invoke(struct alp_inference *h)
{
	(void)h;
	return ALP_OK;
}
void alp_inference_deepx_close(struct alp_inference *h)
{
	(void)h;
}
alp_status_t alp_inference_deepx_bind_cores(struct alp_inference *h, unsigned bound)
{
	(void)h;
	(void)bound;
	return ALP_OK;
}
alp_status_t alp_inference_deepx_get_status(struct alp_inference *h, alp_deepx_device_status_t *out)
{
	(void)h;
	(void)out;
	return ALP_OK;
}

static const uint8_t k_model[16] = { 'D', 'X', 'N', 'N' };

static alp_inference_config_t cfg_with(alp_inference_backend_t be, alp_inference_model_format_t fmt)
{
	alp_inference_config_t cfg = {
		.model_data = k_model,
		.model_size = sizeof(k_model),
		.format     = fmt,
		.backend    = be,
	};
	return cfg;
}

static void test_bad_arguments_are_invalid(void)
{
	g_open_calls = 0;
	alp_inference_config_t cfg =
	    cfg_with(ALP_INFERENCE_BACKEND_DEEPX_DXM1, ALP_INFERENCE_MODEL_DXNN);

	ALP_ASSERT_NULL(alp_deepx_inference_open(NULL, ALP_DEEPX_NPU_CORES_ALL));
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);

	/* One past the last valid core set (ALP_DEEPX_NPU_CORES_02). */
	ALP_ASSERT_NULL(alp_deepx_inference_open(&cfg, (alp_deepx_npu_cores_t)7u));
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);

	cfg = cfg_with(ALP_INFERENCE_BACKEND_DRPAI, ALP_INFERENCE_MODEL_DXNN);
	ALP_ASSERT_NULL(alp_deepx_inference_open(&cfg, ALP_DEEPX_NPU_CORES_ALL));
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);

	cfg = cfg_with(ALP_INFERENCE_BACKEND_DEEPX_DXM1, ALP_INFERENCE_MODEL_ONNX);
	ALP_ASSERT_NULL(alp_deepx_inference_open(&cfg, ALP_DEEPX_NPU_CORES_ALL));
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);

	ALP_ASSERT_EQ_INT(g_open_calls, 0); /* none reached the backend */
}

static void test_core_set_reaches_backend_and_auto_is_deepx(void)
{
	alp_inference_config_t cfg = cfg_with(ALP_INFERENCE_BACKEND_AUTO, ALP_INFERENCE_MODEL_DXNN);

	g_open_calls       = 0;
	alp_inference_t *h = alp_deepx_inference_open(&cfg, ALP_DEEPX_NPU_CORES_12);
	ALP_ASSERT_TRUE(h != NULL);
	ALP_ASSERT_EQ_INT(g_open_calls, 1);
	ALP_ASSERT_EQ_INT((int)g_last_bound, (int)ALP_DEEPX_NPU_CORES_12);
	ALP_ASSERT_EQ_INT(h->backend, ALP_INFERENCE_BACKEND_DEEPX_DXM1); /* AUTO pinned */
	alp_inference_close(h);

	/* Plain alp_inference_open() means "all cores". */
	cfg = cfg_with(ALP_INFERENCE_BACKEND_DEEPX_DXM1, ALP_INFERENCE_MODEL_DXNN);
	h   = alp_inference_open(&cfg);
	ALP_ASSERT_TRUE(h != NULL);
	ALP_ASSERT_EQ_INT((int)g_last_bound, (int)ALP_DEEPX_NPU_CORES_ALL);
	alp_inference_close(h);
}

static void test_full_pool_is_nomem(void)
{
	alp_inference_config_t cfg =
	    cfg_with(ALP_INFERENCE_BACKEND_DEEPX_DXM1, ALP_INFERENCE_MODEL_DXNN);
	alp_inference_t *hs[ALP_SDK_MAX_INFERENCE_HANDLES];

	for (int i = 0; i < ALP_SDK_MAX_INFERENCE_HANDLES; ++i) {
		hs[i] = alp_deepx_inference_open(&cfg, ALP_DEEPX_NPU_CORE_0);
		ALP_ASSERT_TRUE(hs[i] != NULL);
	}
	ALP_ASSERT_NULL(alp_deepx_inference_open(&cfg, ALP_DEEPX_NPU_CORE_0));
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_NOMEM);

	for (int i = 0; i < ALP_SDK_MAX_INFERENCE_HANDLES; ++i) {
		alp_inference_close(hs[i]);
	}
	/* A slot is free again. */
	alp_inference_t *h = alp_deepx_inference_open(&cfg, ALP_DEEPX_NPU_CORE_0);
	ALP_ASSERT_TRUE(h != NULL);
	alp_inference_close(h);
}

int main(void)
{
	test_bad_arguments_are_invalid();
	test_core_set_reaches_backend_and_auto_is_deepx();
	test_full_pool_is_nomem();

	ALP_TEST_SUMMARY();
}
