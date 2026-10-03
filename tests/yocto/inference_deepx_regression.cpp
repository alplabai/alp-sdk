/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for src/yocto/inference_deepx.cpp's tensor-rank
 * guards (issue #1729):
 *
 *   1. open()-time: all_tensor_ranks_fit() refuses (ALP_ERR_NOSUPPORT) a
 *      model whose DECLARED (GetInputs()/GetOutputs()) tensor rank exceeds
 *      alp_inference_tensor_t's 4-slot shape[].  Pre-fix, open() returned
 *      ALP_OK and get_input()/get_output() silently truncated the shape.
 *   2. LIVE-path: alp_inference_deepx_get_output() re-checks the rank of
 *      the tensor a real Run() hands back (st->last_outputs).  The
 *      open()-time check above does NOT cover this: it only sees the
 *      DECLARED metadata (GetOutputs()), not what invoke() actually
 *      returns.  A model whose live output rank differs from its declared
 *      rank (e.g. a dynamic/data-dependent shape) used to reach
 *      fill_tensor_descriptor() on this path with no guard at all.
 *
 * dx_rt is proprietary (DEEPX EULA) and not available in this repo or in
 * CI (issue #1747) -- these tests compile src/yocto/inference_deepx.cpp
 * directly against tests/yocto/fakes/dxrt/, a clean-room stand-in (see its
 * datatype.h) that gives the test full control over what the "device"
 * reports, so inference_deepx.cpp's OWN C++ logic is exercised
 * deterministically without any real DX-M1 hardware.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_inference_deepx_regression
 *   ctest --test-dir build -R alp_test_inference_deepx_regression
 */

#include <cstdint>
#include <utility>
#include <vector>

#include "dxrt/inference_engine.h" /* pulls in dxrt_test:: + dxrt::Tensor/Tensors */

extern "C" {
#include "alp/inference.h"
}

#include "inference_handle_internal.h"
#include "test_assert.h"

/* Forward declarations -- mirrors src/yocto/inference_yocto.c's own
 * #if defined(ALP_SDK_USE_DEEPX_DXM1) block; this test links
 * inference_deepx.cpp directly rather than through the dispatcher, so it
 * declares the same hook prototypes itself. */
extern "C" {
alp_status_t alp_inference_deepx_open(struct alp_inference         *h,
                                      const alp_inference_config_t *cfg,
                                      unsigned                      bound);
alp_status_t alp_inference_deepx_bind_cores(struct alp_inference *h, unsigned bound);
alp_status_t
alp_inference_deepx_get_output(struct alp_inference *h, size_t index, alp_inference_tensor_t *out);
alp_status_t alp_inference_deepx_invoke(struct alp_inference *h);
void         alp_inference_deepx_close(struct alp_inference *h);
}

namespace
{

static uint8_t s_scratch[4] = { 0, 0, 0, 0 };

dxrt::Tensor make_tensor(std::vector<int64_t> shape)
{
	dxrt::Tensor t;
	t.shape_      = std::move(shape);
	t.type_       = dxrt::FLOAT;
	t.size_bytes_ = sizeof(s_scratch);
	t.data_       = s_scratch;
	return t;
}

void reset_fakes()
{
	dxrt_test::g_declared_inputs.clear();
	dxrt_test::g_declared_outputs.clear();
	dxrt_test::g_run_outputs.clear();
	dxrt_test::g_run_should_throw = false;
	dxrt_test::g_ctor_bounds.clear();
}

const uint8_t          k_dummy_model[4] = { 'D', 'X', 'N', 'N' };
alp_inference_config_t base_cfg()
{
	alp_inference_config_t cfg = {};
	cfg.model_data             = k_dummy_model;
	cfg.model_size             = sizeof(k_dummy_model);
	cfg.format                 = ALP_INFERENCE_MODEL_DXNN;
	cfg.backend                = ALP_INFERENCE_BACKEND_DEEPX_DXM1;
	return cfg;
}

/* Test 1: open()-time refusal of a rank-5 DECLARED input (issue #1729). */
void test_open_refuses_declared_rank_over_4()
{
	reset_fakes();
	dxrt_test::g_declared_inputs.push_back(make_tensor({ 1, 2, 3, 4, 5 })); /* rank 5 */
	dxrt_test::g_declared_outputs.push_back(make_tensor({ 1, 1 }));         /* rank 2, fine */

	struct alp_inference   h   = {};
	alp_inference_config_t cfg = base_cfg();
	alp_status_t           rc  = alp_inference_deepx_open(&h, &cfg, 0u);

	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOSUPPORT);
	ALP_ASSERT_NULL(h.be_state);
}

/* Test 1b: a model whose declared ranks all fit opens fine (no false
 * positive from the guard). */
void test_open_accepts_declared_rank_at_4()
{
	reset_fakes();
	dxrt_test::g_declared_inputs.push_back(make_tensor({ 1, 2, 3, 4 })); /* rank 4, fine */
	dxrt_test::g_declared_outputs.push_back(make_tensor({ 1, 1 }));      /* rank 2, fine */

	struct alp_inference   h   = {};
	alp_inference_config_t cfg = base_cfg();
	alp_status_t           rc  = alp_inference_deepx_open(&h, &cfg, 0u);

	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_TRUE(h.be_state != nullptr);
	alp_inference_deepx_close(&h);
}

/* Test 2: the LIVE post-Run() rank guard in get_output() (this change,
 * issue #1729/#3).  Declared output rank is 2 (passes open()'s check);
 * Run() hands back a rank-5 tensor for that same output -- get_output()
 * must refuse it, not truncate. */
void test_get_output_refuses_live_rank_over_4()
{
	reset_fakes();
	dxrt_test::g_declared_inputs.push_back(make_tensor({ 1, 4 }));
	dxrt_test::g_declared_outputs.push_back(make_tensor({ 1, 1 })); /* declared: rank 2 */

	struct alp_inference   h   = {};
	alp_inference_config_t cfg = base_cfg();
	ALP_ASSERT_EQ_INT(alp_inference_deepx_open(&h, &cfg, 0u), ALP_OK);

	static dxrt::Tensor live_out = make_tensor({ 1, 2, 3, 4, 5 }); /* live: rank 5 */
	dxrt_test::g_run_outputs.push_back(&live_out);

	ALP_ASSERT_EQ_INT(alp_inference_deepx_invoke(&h), ALP_OK);

	alp_inference_tensor_t out = {};
	alp_status_t           rc  = alp_inference_deepx_get_output(&h, 0, &out);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOSUPPORT);

	alp_inference_deepx_close(&h);
}

/* Test 2b: a live output whose rank matches the declared one (<=4) still
 * round-trips normally (no false positive from the live-path guard). */
void test_get_output_accepts_live_rank_at_4()
{
	reset_fakes();
	dxrt_test::g_declared_inputs.push_back(make_tensor({ 1, 4 }));
	dxrt_test::g_declared_outputs.push_back(make_tensor({ 1, 1 }));

	struct alp_inference   h   = {};
	alp_inference_config_t cfg = base_cfg();
	ALP_ASSERT_EQ_INT(alp_inference_deepx_open(&h, &cfg, 0u), ALP_OK);

	static dxrt::Tensor live_out = make_tensor({ 1, 2, 3, 4 }); /* live: rank 4 */
	dxrt_test::g_run_outputs.push_back(&live_out);

	ALP_ASSERT_EQ_INT(alp_inference_deepx_invoke(&h), ALP_OK);

	alp_inference_tensor_t out = {};
	ALP_ASSERT_EQ_INT(alp_inference_deepx_get_output(&h, 0, &out), ALP_OK);
	ALP_ASSERT_EQ_INT(out.rank, 4);

	alp_inference_deepx_close(&h);
}

/* Test 3: open()-time refusal of a multi-input model (issue #1645).
 * invoke() hands dx_rt's Run() a single pointer -- st->input_bufs[0].data()
 * -- that dx_rt reads sum(size_in_bytes()) bytes from, treating it as one
 * contiguous concatenated blob.  This backend stages each input in its own
 * SEPARATE std::vector allocation, so for any model declaring more than one
 * input the real bytes read would run past input_bufs[0]'s allocation and
 * DMA whatever unrelated heap memory follows it over PCIe.  open() must
 * refuse before invoke() can ever reach that path. */
void test_open_refuses_multi_input_model()
{
	reset_fakes();
	dxrt_test::g_declared_inputs.push_back(make_tensor({ 1, 4 }));
	dxrt_test::g_declared_inputs.push_back(make_tensor({ 1, 4 })); /* 2nd input */
	dxrt_test::g_declared_outputs.push_back(make_tensor({ 1, 1 }));

	struct alp_inference   h   = {};
	alp_inference_config_t cfg = base_cfg();
	alp_status_t           rc  = alp_inference_deepx_open(&h, &cfg, 0u);

	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOSUPPORT);
	ALP_ASSERT_NULL(h.be_state);
}

/* Test 3b: a single-input model -- the common case -- still opens fine (no
 * false positive from the guard). */
void test_open_accepts_single_input_model()
{
	reset_fakes();
	dxrt_test::g_declared_inputs.push_back(make_tensor({ 1, 4 }));
	dxrt_test::g_declared_outputs.push_back(make_tensor({ 1, 1 }));

	struct alp_inference   h   = {};
	alp_inference_config_t cfg = base_cfg();
	alp_status_t           rc  = alp_inference_deepx_open(&h, &cfg, 0u);

	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_TRUE(h.be_state != nullptr);
	alp_inference_deepx_close(&h);
}

/* Test 4: the engine is built directly on the requested cores -- no
 * transient all-cores (boundOption 0) engine (#2398 hypothesis). */
void test_open_builds_engine_on_requested_cores_only()
{
	reset_fakes();
	dxrt_test::g_declared_inputs.push_back(make_tensor({ 1, 4 }));
	dxrt_test::g_declared_outputs.push_back(make_tensor({ 1, 1 }));

	struct alp_inference   h   = {};
	alp_inference_config_t cfg = base_cfg();
	ALP_ASSERT_EQ_INT(alp_inference_deepx_open(&h, &cfg, 3u), ALP_OK); /* NPU_2 */
	ALP_ASSERT_EQ_INT((int)dxrt_test::g_ctor_bounds.size(), 1);
	ALP_ASSERT_EQ_INT((int)dxrt_test::g_ctor_bounds[0], 3);
	alp_inference_deepx_close(&h);
}

/* Test 5: at most 3 distinct core sets per process; a 4th is ALP_ERR_BUSY,
 * the same set is shared, and closing frees a slot. */
void test_fourth_distinct_core_set_is_busy()
{
	reset_fakes();
	dxrt_test::g_declared_inputs.push_back(make_tensor({ 1, 4 }));
	dxrt_test::g_declared_outputs.push_back(make_tensor({ 1, 1 }));

	struct alp_inference   h[5] = {};
	alp_inference_config_t cfg  = base_cfg();
	ALP_ASSERT_EQ_INT(alp_inference_deepx_open(&h[0], &cfg, 1u), ALP_OK);
	ALP_ASSERT_EQ_INT(alp_inference_deepx_open(&h[1], &cfg, 2u), ALP_OK);
	ALP_ASSERT_EQ_INT(alp_inference_deepx_open(&h[2], &cfg, 3u), ALP_OK);
	/* 4th distinct set: refused, no engine built for it. */
	const size_t built = dxrt_test::g_ctor_bounds.size();
	ALP_ASSERT_EQ_INT(alp_inference_deepx_open(&h[3], &cfg, 4u), ALP_ERR_BUSY);
	ALP_ASSERT_EQ_INT((int)dxrt_test::g_ctor_bounds.size(), (int)built);
	/* A set already live is shared, not counted again. */
	ALP_ASSERT_EQ_INT(alp_inference_deepx_open(&h[3], &cfg, 2u), ALP_OK);
	/* bind_cores to a new set would be a 4th too. */
	ALP_ASSERT_EQ_INT(alp_inference_deepx_bind_cores(&h[0], 4u), ALP_ERR_BUSY);

	/* Free set 1 (h[0] is its only user): a new set fits again. */
	alp_inference_deepx_close(&h[0]);
	ALP_ASSERT_EQ_INT(alp_inference_deepx_open(&h[4], &cfg, 4u), ALP_OK);

	alp_inference_deepx_close(&h[1]);
	alp_inference_deepx_close(&h[2]);
	alp_inference_deepx_close(&h[3]);
	alp_inference_deepx_close(&h[4]);
}

/* Test 6: a failed open (bad model rank) gives its core-set slot back. */
void test_failed_open_releases_core_slot()
{
	reset_fakes();
	dxrt_test::g_declared_inputs.push_back(make_tensor({ 1, 2, 3, 4, 5 })); /* rank 5 */
	dxrt_test::g_declared_outputs.push_back(make_tensor({ 1, 1 }));

	struct alp_inference   h   = {};
	alp_inference_config_t cfg = base_cfg();
	for (int i = 0; i < 6; ++i) { /* would exhaust 3 slots if each leaked */
		ALP_ASSERT_EQ_INT(alp_inference_deepx_open(&h, &cfg, 1u + (unsigned)(i % 3)),
		                  ALP_ERR_NOSUPPORT);
	}
	for (int b = 0; b < 8; ++b) {
		ALP_ASSERT_EQ_INT(dxrt_test::g_live_by_bound[b], 0);
	}
}

} /* namespace */

int main(void)
{
	test_open_refuses_declared_rank_over_4();
	test_open_accepts_declared_rank_at_4();
	test_get_output_refuses_live_rank_over_4();
	test_get_output_accepts_live_rank_at_4();
	test_open_refuses_multi_input_model();
	test_open_accepts_single_input_model();
	test_open_builds_engine_on_requested_cores_only();
	test_fourth_distinct_core_set_is_busy();
	test_failed_open_releases_core_slot();

	ALP_TEST_SUMMARY();
}
