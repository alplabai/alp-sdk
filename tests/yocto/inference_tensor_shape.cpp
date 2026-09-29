/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for src/yocto/inference_tensor_shape.h -- the
 * rank/shape-copy helper shared by the ORT (inference_ort.cpp) and DEEPX
 * (inference_deepx.cpp) Yocto inference backends (issue #1729).  Neither
 * backend TU compiles under any local or CI gate today (both are gated by
 * a CMake option that defaults OFF, and no workflow turns either on), so
 * this header -- which has ZERO dependency on the ONNX Runtime C API or
 * dx_rt -- is where the rank > 4 refusal is actually exercised. Links with
 * no vendor SDK and no NPU hardware.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_inference_tensor_shape
 *   ctest --test-dir build -R alp_test_inference_tensor_shape
 */

#include <cstdint>
#include <vector>

#include "test_assert.h"

#include "../../src/yocto/inference_tensor_shape.h"

using alp_inference_shape::fill_fixed_shape;

/** #1: a normal rank-4 tensor copies straight through. */
static void test_rank4_copies_through(void)
{
	const int64_t          dims[4] = { 1, 3, 640, 640 };
	alp_inference_tensor_t out{};
	bool                   ok = fill_fixed_shape(dims, 4, &out);

	ALP_ASSERT_TRUE(ok);
	ALP_ASSERT_EQ_INT(out.rank, 4);
	ALP_ASSERT_EQ_INT(out.shape[0], 1);
	ALP_ASSERT_EQ_INT(out.shape[1], 3);
	ALP_ASSERT_EQ_INT(out.shape[2], 640);
	ALP_ASSERT_EQ_INT(out.shape[3], 640);
}

/** #2: rank < 4 copies its dims and zero-fills the rest -- a caller must
 *  never see stale/uninitialised trailing entries. */
static void test_rank_below_4_zero_fills_remainder(void)
{
	const int64_t          dims[2] = { 1, 1000 };
	alp_inference_tensor_t out;
	out.shape[2] = 0xBEEF; /* poison -- must be overwritten with 0 */
	out.shape[3] = 0xBEEF;
	bool ok      = fill_fixed_shape(dims, 2, &out);

	ALP_ASSERT_TRUE(ok);
	ALP_ASSERT_EQ_INT(out.rank, 2);
	ALP_ASSERT_EQ_INT(out.shape[0], 1);
	ALP_ASSERT_EQ_INT(out.shape[1], 1000);
	ALP_ASSERT_EQ_INT(out.shape[2], 0);
	ALP_ASSERT_EQ_INT(out.shape[3], 0);
}

/** #3: rank 0 (scalar) is a legitimate, non-refused shape. */
static void test_rank0_is_not_refused(void)
{
	alp_inference_tensor_t out;
	bool                   ok = fill_fixed_shape(nullptr, 0, &out);

	ALP_ASSERT_TRUE(ok);
	ALP_ASSERT_EQ_INT(out.rank, 0);
	ALP_ASSERT_EQ_INT(out.shape[0], 0);
	ALP_ASSERT_EQ_INT(out.shape[1], 0);
	ALP_ASSERT_EQ_INT(out.shape[2], 0);
	ALP_ASSERT_EQ_INT(out.shape[3], 0);
}

/** #4: THE issue #1729 case -- a rank-5 tensor must be REFUSED (false),
 *  not silently truncated to its first 4 dims.  @p out is left entirely
 *  untouched on refusal, proven here by poisoning it first. */
static void test_rank5_is_refused_not_truncated(void)
{
	const int64_t          dims[5] = { 1, 3, 4, 5, 6 };
	alp_inference_tensor_t out;
	out.rank     = 0xAA;
	out.shape[0] = 0xDEAD;
	out.shape[1] = 0xDEAD;
	out.shape[2] = 0xDEAD;
	out.shape[3] = 0xDEAD;
	bool ok      = fill_fixed_shape(dims, 5, &out);

	ALP_ASSERT_TRUE(!ok);
	/* untouched -- not truncated to {1,3,4,5} */
	ALP_ASSERT_EQ_INT(out.rank, 0xAA);
	ALP_ASSERT_EQ_INT(out.shape[0], 0xDEAD);
}

/** #5: a much-higher rank (e.g. an unusual custom op) is refused the
 *  same way -- not just the "one over" boundary. */
static void test_rank8_is_refused(void)
{
	const int64_t          dims[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	alp_inference_tensor_t out;
	bool                   ok = fill_fixed_shape(dims, 8, &out);

	ALP_ASSERT_TRUE(!ok);
}

/** #6: a dim that does not fit uint16_t is the SAME defect one level down.
 *  inference_deepx.cpp had no bounds check, so 100000 wrapped to 34464 --
 *  a plausible-looking wrong extent. inference_ort.cpp already rejected
 *  this at load time; the helper now covers both (#1729, #1646 class). */
static void test_dim_above_uint16_is_refused(void)
{
	const int64_t          dims[2] = { 1, 100000 };
	alp_inference_tensor_t out;
	out.rank     = 0xAA;
	out.shape[0] = 0xDEAD;
	bool ok      = fill_fixed_shape(dims, 2, &out);

	ALP_ASSERT_TRUE(!ok);
	ALP_ASSERT_EQ_INT(out.rank, 0xAA);
	ALP_ASSERT_EQ_INT(out.shape[0], 0xDEAD);
}

/** #7: a symbolic/dynamic dim arrives as -1. Casting it would yield 65535.
 *  Refuse rather than invent an extent -- inference_ort.cpp pins such dims
 *  to 1 at gather time with its own documented rationale; this helper does
 *  not silently guess on a backend that never made that choice. */
static void test_negative_dim_is_refused(void)
{
	const int64_t          dims[2] = { 1, -1 };
	alp_inference_tensor_t out;
	out.rank     = 0xAA;
	out.shape[0] = 0xDEAD;
	bool ok      = fill_fixed_shape(dims, 2, &out);

	ALP_ASSERT_TRUE(!ok);
	ALP_ASSERT_EQ_INT(out.rank, 0xAA);
	ALP_ASSERT_EQ_INT(out.shape[0], 0xDEAD);
}

/** #8: the boundary itself is representable and must NOT be refused. */
static void test_dim_at_uint16_max_passes(void)
{
	const int64_t          dims[2] = { 1, 65535 };
	alp_inference_tensor_t out;
	bool                   ok = fill_fixed_shape(dims, 2, &out);

	ALP_ASSERT_TRUE(ok);
	ALP_ASSERT_EQ_INT(out.shape[1], 65535);
}

int main(void)
{
	test_rank4_copies_through();
	test_rank_below_4_zero_fills_remainder();
	test_rank0_is_not_refused();
	test_rank5_is_refused_not_truncated();
	test_rank8_is_refused();
	test_dim_above_uint16_is_refused();
	test_negative_dim_is_refused();
	test_dim_at_uint16_max_passes();

	ALP_TEST_SUMMARY();
}
