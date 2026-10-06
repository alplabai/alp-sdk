/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression for alp-sdk#2389 on the Yocto inference dispatcher
 * (src/yocto/inference_yocto.c): alp_inference_open() used to hand
 * cfg->model_data straight to whatever backend AUTO/the caller resolved
 * to, with no check that cfg->format matches what that backend actually
 * parses.  On an NPU-bearing SoM with DRP-AI compiled in, AUTO resolves
 * to DRP-AI by design (resolve_auto()'s own header comment: an NPU-
 * bearing SoM must never silently fall to CPU) -- so a `.onnx` blob
 * opened with backend=AUTO used to be piped straight into DRP-AI's
 * `tar -xf -` extractor with nothing rejecting the mismatch first.
 *
 * Drives the REAL public alp_inference_open() by #including
 * src/yocto/inference_yocto.c directly into this TU (same technique as
 * tests/yocto/inference_invoke_close_race.c) through TWO fake backends
 * -- DRPAI and CPU -- forced on before the #include so the dispatcher's
 * real resolve_auto()/format-gate/switch code all run unmodified; only
 * the two backends' "open" hooks are faked, each counting its own call
 * so a test can prove a rejected format never reached a backend at all.
 * Single-threaded (no race under test), so no pthreads dependency --
 * portable to every host this test suite builds on, no Linux gate.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_inference_format_gate
 *   ctest --test-dir build -R alp_test_inference_format_gate
 */

#include <stdint.h>

#include "test_assert.h"

#define ALP_SDK_USE_DRPAI_V2N 1
#define ALP_SDK_USE_ORT_CPU   1
#include "../../src/yocto/inference_yocto.c"

static int g_drpai_open_calls;
static int g_ort_open_calls;

alp_status_t alp_inference_drpai_open(struct alp_inference *h, const alp_inference_config_t *cfg)
{
	(void)cfg;
	g_drpai_open_calls++;
	h->be_state = NULL;
	return ALP_OK;
}
size_t alp_inference_drpai_num_inputs(struct alp_inference *h)
{
	(void)h;
	return 0u;
}
size_t alp_inference_drpai_num_outputs(struct alp_inference *h)
{
	(void)h;
	return 0u;
}
alp_status_t
alp_inference_drpai_get_input(struct alp_inference *h, size_t index, alp_inference_tensor_t *out)
{
	(void)h;
	(void)index;
	*out = (alp_inference_tensor_t){ 0 };
	return ALP_OK;
}
alp_status_t
alp_inference_drpai_get_output(struct alp_inference *h, size_t index, alp_inference_tensor_t *out)
{
	(void)h;
	(void)index;
	*out = (alp_inference_tensor_t){ 0 };
	return ALP_OK;
}
alp_status_t alp_inference_drpai_invoke(struct alp_inference *h)
{
	(void)h;
	return ALP_OK;
}
void alp_inference_drpai_close(struct alp_inference *h)
{
	(void)h;
}

alp_status_t alp_inference_ort_open(struct alp_inference *h, const alp_inference_config_t *cfg)
{
	(void)cfg;
	g_ort_open_calls++;
	h->be_state = NULL;
	return ALP_OK;
}
size_t alp_inference_ort_num_inputs(struct alp_inference *h)
{
	(void)h;
	return 0u;
}
size_t alp_inference_ort_num_outputs(struct alp_inference *h)
{
	(void)h;
	return 0u;
}
alp_status_t
alp_inference_ort_get_input(struct alp_inference *h, size_t index, alp_inference_tensor_t *out)
{
	(void)h;
	(void)index;
	*out = (alp_inference_tensor_t){ 0 };
	return ALP_OK;
}
alp_status_t
alp_inference_ort_get_output(struct alp_inference *h, size_t index, alp_inference_tensor_t *out)
{
	(void)h;
	(void)index;
	*out = (alp_inference_tensor_t){ 0 };
	return ALP_OK;
}
alp_status_t alp_inference_ort_invoke(struct alp_inference *h)
{
	(void)h;
	return ALP_OK;
}
void alp_inference_ort_close(struct alp_inference *h)
{
	(void)h;
}

static const uint8_t k_model[16] = { 0xDE, 0xAD, 0xBE, 0xEF };

static void reset_call_counters(void)
{
	g_drpai_open_calls = 0;
	g_ort_open_calls   = 0;
}

/* AUTO resolves from the model FORMAT: an ONNX blob goes to the CPU/ORT
 * backend, never into DRP-AI's tar extractor (the original repro). */
static void test_auto_routes_onnx_format_to_cpu_not_drpai(void)
{
	reset_call_counters();

	alp_inference_config_t cfg = {
		.model_data = k_model,
		.model_size = sizeof(k_model),
		.format     = ALP_INFERENCE_MODEL_ONNX,
		.backend    = ALP_INFERENCE_BACKEND_AUTO,
	};
	alp_inference_t *h = alp_inference_open(&cfg);

	ALP_ASSERT_TRUE(h != NULL);
	ALP_ASSERT_EQ_INT(g_ort_open_calls, 1);
	ALP_ASSERT_EQ_INT(g_drpai_open_calls, 0);
	alp_inference_close(h);
}

/* A format whose NPU is not compiled in (DXNN, no DX-M1 here) is NOSUPPORT
 * under AUTO -- it never falls through to another backend. */
static void test_auto_dxnn_without_dxm1_is_nosupport(void)
{
	reset_call_counters();

	alp_inference_config_t cfg = {
		.model_data = k_model,
		.model_size = sizeof(k_model),
		.format     = ALP_INFERENCE_MODEL_DXNN,
		.backend    = ALP_INFERENCE_BACKEND_AUTO,
	};
	ALP_ASSERT_NULL(alp_inference_open(&cfg));
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_NOSUPPORT);
	ALP_ASSERT_EQ_INT(g_drpai_open_calls, 0);
	ALP_ASSERT_EQ_INT(g_ort_open_calls, 0);
}

/* Same AUTO resolution to DRP-AI via a DRPAI format -- must still open
 * normally through the resolved backend. */
static void test_auto_resolves_to_drpai_and_accepts_matching_format(void)
{
	reset_call_counters();

	alp_inference_config_t cfg = {
		.model_data = k_model,
		.model_size = sizeof(k_model),
		.format     = ALP_INFERENCE_MODEL_DRPAI,
		.backend    = ALP_INFERENCE_BACKEND_AUTO,
	};
	alp_inference_t *h = alp_inference_open(&cfg);

	ALP_ASSERT_TRUE(h != NULL);
	ALP_ASSERT_EQ_INT(g_drpai_open_calls, 1);
	alp_inference_close(h);
}

/* A caller who explicitly pins the ORT CPU backend with an ONNX blob
 * must still be able to open it -- AUTO's NPU-first policy must not
 * block an explicit CPU selection (per the issue's own "expected"
 * behaviour: pinning CPU is the caller's way to opt out of the NPU). */
static void test_explicit_cpu_pin_with_onnx_still_opens(void)
{
	reset_call_counters();

	alp_inference_config_t cfg = {
		.model_data = k_model,
		.model_size = sizeof(k_model),
		.format     = ALP_INFERENCE_MODEL_ONNX,
		.backend    = ALP_INFERENCE_BACKEND_CPU,
	};
	alp_inference_t *h = alp_inference_open(&cfg);

	ALP_ASSERT_TRUE(h != NULL);
	ALP_ASSERT_EQ_INT(g_ort_open_calls, 1);
	ALP_ASSERT_EQ_INT(g_drpai_open_calls, 0);
	alp_inference_close(h);
}

/* An explicit (non-AUTO) DRPAI pin with the wrong format is rejected the
 * same way AUTO's resolved pick is -- the gate applies uniformly, not
 * only on the AUTO path. */
static void test_explicit_drpai_pin_with_onnx_rejected(void)
{
	reset_call_counters();

	alp_inference_config_t cfg = {
		.model_data = k_model,
		.model_size = sizeof(k_model),
		.format     = ALP_INFERENCE_MODEL_ONNX,
		.backend    = ALP_INFERENCE_BACKEND_DRPAI,
	};
	alp_inference_t *h = alp_inference_open(&cfg);

	ALP_ASSERT_NULL(h);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
	ALP_ASSERT_EQ_INT(g_drpai_open_calls, 0);
}

/* The mismatch direction the issue's OWN repro did not hit but the gate
 * must reject the same way: an explicit CPU pin with a DRPAI-formatted
 * blob.  ORT's open() must never be called either. */
static void test_explicit_cpu_pin_with_drpai_format_rejected(void)
{
	reset_call_counters();

	alp_inference_config_t cfg = {
		.model_data = k_model,
		.model_size = sizeof(k_model),
		.format     = ALP_INFERENCE_MODEL_DRPAI,
		.backend    = ALP_INFERENCE_BACKEND_CPU,
	};
	alp_inference_t *h = alp_inference_open(&cfg);

	ALP_ASSERT_NULL(h);
	ALP_ASSERT_EQ_INT(alp_last_error(), ALP_ERR_INVAL);
	ALP_ASSERT_EQ_INT(g_ort_open_calls, 0);
	ALP_ASSERT_EQ_INT(g_drpai_open_calls, 0);
}

int main(void)
{
	test_auto_routes_onnx_format_to_cpu_not_drpai();
	test_auto_dxnn_without_dxm1_is_nosupport();
	test_auto_resolves_to_drpai_and_accepts_matching_format();
	test_explicit_cpu_pin_with_onnx_still_opens();
	test_explicit_drpai_pin_with_onnx_rejected();
	test_explicit_cpu_pin_with_drpai_format_rejected();

	ALP_TEST_SUMMARY();
}
