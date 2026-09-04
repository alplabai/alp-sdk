/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for src/yocto/inference_ort.cpp's open() path:
 *
 *   1. Tensor-rank refusal (issue #1729) -- and the follow-up split of
 *      that single check into TWO separate ceilings: a hostile/corrupt
 *      ndim (kHostileTensorRankCeiling, ALP_ERR_INVAL) is a different
 *      condition from a well-formed model this 4-slot descriptor simply
 *      cannot represent (kMaxTensorRank, ALP_ERR_NOSUPPORT).  A prior
 *      revision of this fix collapsed both into one check (kMaxTensorRank
 *      lowered from 32 to 4, INVAL upgraded to NOSUPPORT for that single
 *      branch) -- which correctly refuses an unrepresentable-but-legit
 *      rank-5..32 tensor, but ALSO reports a hostile rank-1000 ndim as
 *      merely "unrepresentable" (NOSUPPORT) instead of "malformed"
 *      (INVAL).  test_open_rejects_hostile_rank_as_inval below is the
 *      one that catches that collapse.
 *
 *   2. OrtValue release on open()'s throw path (issue #1494): every
 *      OrtValue already created before a later step throws must be
 *      released, not just cpu_mem_info/session/env.  A prior revision's
 *      catch(...) handler released those three but NOT the OrtValue
 *      vectors, leaking every OrtValue created before the throw.
 *
 * ONNX Runtime's real C API is staged onto the Yocto sysroot at build
 * time (issue #1747: no PR gate builds this backend, same as DEEPX) and
 * is not installed on this dev host or in CI.  These tests compile
 * src/yocto/inference_ort.cpp directly against
 * tests/yocto/fakes/onnxruntime/onnxruntime_c_api.h, a clean-room stand-in
 * (see that header's own comment) that supplies its own OrtGetApiBase(),
 * so inference_ort.cpp's OWN C++ logic is exercised deterministically
 * without any real ONNX Runtime install.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_inference_ort_regression
 *   ctest --test-dir build -R alp_test_inference_ort_regression
 */

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <vector>

#include "onnxruntime_c_api.h"

extern "C" {
#include "alp/inference.h"
}

#include "inference_handle_internal.h"
#include "test_assert.h"

/* Forward declarations -- mirrors src/yocto/inference_yocto.c's own
 * #if defined(ALP_SDK_USE_ORT_CPU) block; this test links inference_ort.cpp
 * directly rather than through the dispatcher, so it declares the same
 * hook prototype itself. */
extern "C" {
alp_status_t alp_inference_ort_open(struct alp_inference *h, const alp_inference_config_t *cfg);
}

namespace ort_test
{

struct FakeState {
	size_t                            n_in  = 1;
	size_t                            n_out = 1;
	std::vector<std::vector<int64_t>> in_dims;
	std::vector<std::vector<int64_t>> out_dims;

	/* CreateTensorWithDataAsOrtValue is called n_in times (inputs) then
	 * n_out times (outputs), in that order -- see alp_inference_ort_open().
	 * call_index counts across BOTH loops so a test can force a throw at
	 * any specific call, after N values have already been created. */
	int create_value_call_index = 0;
	int throw_at_call_index     = -1;
	int created_value_count     = 0;
	int released_value_count    = 0;
	int released_env_count      = 0;
	int released_session_count  = 0;
	int released_meminfo_count  = 0;
};

inline FakeState g_state;

void reset()
{
	g_state = FakeState{};
}

void *encode_tag(bool is_input, size_t index)
{
	uintptr_t v = (is_input ? 0u : 0x10000u) + index + 1;
	return reinterpret_cast<void *>(v);
}

void decode_tag(const void *tag, bool *is_input, size_t *index)
{
	uintptr_t v = reinterpret_cast<uintptr_t>(tag) - 1;
	*is_input   = v < 0x10000u;
	*index      = *is_input ? v : (v - 0x10000u);
}

} /* namespace ort_test */

namespace
{

extern "C" OrtStatus *fake_CreateEnv(OrtLoggingLevel, const char *, OrtEnv **out)
{
	static int tag;
	*out = reinterpret_cast<OrtEnv *>(&tag);
	return nullptr;
}

extern "C" OrtStatus *fake_CreateSessionOptions(OrtSessionOptions **out)
{
	static int tag;
	*out = reinterpret_cast<OrtSessionOptions *>(&tag);
	return nullptr;
}

extern "C" void fake_ReleaseSessionOptions(OrtSessionOptions *)
{
}

extern "C" OrtStatus *fake_CreateSessionFromArray(const OrtEnv *,
                                                  const void *,
                                                  size_t,
                                                  const OrtSessionOptions *,
                                                  OrtSession **out)
{
	static int tag;
	*out = reinterpret_cast<OrtSession *>(&tag);
	return nullptr;
}

extern "C" OrtStatus *fake_CreateCpuMemoryInfo(OrtAllocatorType, OrtMemType, OrtMemoryInfo **out)
{
	static int tag;
	*out = reinterpret_cast<OrtMemoryInfo *>(&tag);
	return nullptr;
}

extern "C" void *fake_Alloc(OrtAllocator *, size_t size)
{
	return malloc(size);
}

extern "C" void fake_Free(OrtAllocator *, void *p)
{
	free(p);
}

extern "C" const OrtMemoryInfo *fake_Info(const OrtAllocator *)
{
	return nullptr;
}

OrtAllocator g_fake_allocator = { 0, fake_Alloc, fake_Free, fake_Info };

extern "C" OrtStatus *fake_GetAllocatorWithDefaultOptions(OrtAllocator **out)
{
	*out = &g_fake_allocator;
	return nullptr;
}

extern "C" OrtStatus *fake_SessionGetInputCount(const OrtSession *, size_t *out)
{
	*out = ort_test::g_state.n_in;
	return nullptr;
}

extern "C" OrtStatus *fake_SessionGetOutputCount(const OrtSession *, size_t *out)
{
	*out = ort_test::g_state.n_out;
	return nullptr;
}

extern "C" OrtStatus *
fake_SessionGetInputTypeInfo(const OrtSession *, size_t index, OrtTypeInfo **out)
{
	*out = reinterpret_cast<OrtTypeInfo *>(ort_test::encode_tag(true, index));
	return nullptr;
}

extern "C" OrtStatus *
fake_SessionGetOutputTypeInfo(const OrtSession *, size_t index, OrtTypeInfo **out)
{
	*out = reinterpret_cast<OrtTypeInfo *>(ort_test::encode_tag(false, index));
	return nullptr;
}

extern "C" OrtStatus *fake_CastTypeInfoToTensorInfo(const OrtTypeInfo                *type_info,
                                                    const OrtTensorTypeAndShapeInfo **out)
{
	*out = reinterpret_cast<const OrtTensorTypeAndShapeInfo *>(type_info);
	return nullptr;
}

extern "C" OrtStatus *fake_GetTensorElementType(const OrtTensorTypeAndShapeInfo *,
                                                ONNXTensorElementDataType *out)
{
	*out = ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT;
	return nullptr;
}

extern "C" OrtStatus *fake_GetDimensionsCount(const OrtTensorTypeAndShapeInfo *info, size_t *out)
{
	bool   is_input;
	size_t index;
	ort_test::decode_tag(info, &is_input, &index);
	auto &dims = is_input ? ort_test::g_state.in_dims : ort_test::g_state.out_dims;
	*out       = dims[index].size();
	return nullptr;
}

extern "C" OrtStatus *
fake_GetDimensions(const OrtTensorTypeAndShapeInfo *info, int64_t *dim_values, size_t n)
{
	bool   is_input;
	size_t index;
	ort_test::decode_tag(info, &is_input, &index);
	auto &dims = is_input ? ort_test::g_state.in_dims : ort_test::g_state.out_dims;
	for (size_t i = 0; i < n; ++i) {
		dim_values[i] = dims[index][i];
	}
	return nullptr;
}

extern "C" void fake_ReleaseTypeInfo(OrtTypeInfo *)
{
}

extern "C" OrtStatus *
fake_SessionGetInputName(const OrtSession *, size_t index, OrtAllocator *alloc, char **value)
{
	char *p = static_cast<char *>(alloc->Alloc(alloc, 16));
	std::snprintf(p, 16, "in%zu", index);
	*value = p;
	return nullptr;
}

extern "C" OrtStatus *
fake_SessionGetOutputName(const OrtSession *, size_t index, OrtAllocator *alloc, char **value)
{
	char *p = static_cast<char *>(alloc->Alloc(alloc, 16));
	std::snprintf(p, 16, "out%zu", index);
	*value = p;
	return nullptr;
}

extern "C" OrtStatus *fake_CreateTensorWithDataAsOrtValue(const OrtMemoryInfo *,
                                                          void *,
                                                          size_t,
                                                          const int64_t *,
                                                          size_t,
                                                          ONNXTensorElementDataType,
                                                          OrtValue **out)
{
	const int call_index = ort_test::g_state.create_value_call_index++;
	if (call_index == ort_test::g_state.throw_at_call_index) {
		/* Simulates ANY C++ operation between two successful
		 * CreateTensorWithDataAsOrtValue calls throwing (a
		 * std::vector::resize/assign, std::make_unique, or std::string
		 * assignment -- see inference_ort.cpp's own file-header note) --
		 * the RAII fix under test must release every value already
		 * created up to this point regardless of WHERE the throw
		 * originates. */
		throw std::runtime_error("fake ORT CreateTensorWithDataAsOrtValue failure");
	}
	++ort_test::g_state.created_value_count;
	*out = reinterpret_cast<OrtValue *>(new int);
	return nullptr;
}

extern "C" void fake_ReleaseValue(OrtValue *v)
{
	++ort_test::g_state.released_value_count;
	delete reinterpret_cast<int *>(v);
}

extern "C" void fake_ReleaseMemoryInfo(OrtMemoryInfo *)
{
	++ort_test::g_state.released_meminfo_count;
}

extern "C" void fake_ReleaseSession(OrtSession *)
{
	++ort_test::g_state.released_session_count;
}

extern "C" void fake_ReleaseEnv(OrtEnv *)
{
	++ort_test::g_state.released_env_count;
}

extern "C" OrtErrorCode fake_GetErrorCode(const OrtStatus *)
{
	return ORT_OK;
}

extern "C" const char *fake_GetErrorMessage(const OrtStatus *)
{
	return "";
}

extern "C" void fake_ReleaseStatus(OrtStatus *)
{
}

extern "C" OrtStatus *fake_Run(OrtSession *,
                               const OrtRunOptions *,
                               const char *const *,
                               const OrtValue *const *,
                               size_t,
                               const char *const *,
                               size_t,
                               OrtValue **)
{
	return nullptr;
}

OrtApi g_fake_api = {
	fake_CreateEnv,
	fake_CreateSessionOptions,
	fake_ReleaseSessionOptions,
	fake_CreateSessionFromArray,
	fake_CreateCpuMemoryInfo,
	fake_GetAllocatorWithDefaultOptions,
	fake_SessionGetInputCount,
	fake_SessionGetOutputCount,
	fake_SessionGetInputTypeInfo,
	fake_SessionGetOutputTypeInfo,
	fake_CastTypeInfoToTensorInfo,
	fake_GetTensorElementType,
	fake_GetDimensionsCount,
	fake_GetDimensions,
	fake_ReleaseTypeInfo,
	fake_SessionGetInputName,
	fake_SessionGetOutputName,
	fake_CreateTensorWithDataAsOrtValue,
	fake_ReleaseValue,
	fake_ReleaseMemoryInfo,
	fake_ReleaseSession,
	fake_ReleaseEnv,
	fake_GetErrorCode,
	fake_GetErrorMessage,
	fake_ReleaseStatus,
	fake_Run,
};

extern "C" const OrtApi *fake_GetApi(uint32_t)
{
	return &g_fake_api;
}

extern "C" const char *fake_GetVersionString()
{
	return "fake-onnxruntime-test-double";
}

OrtApiBase g_fake_api_base = { fake_GetApi, fake_GetVersionString };

const uint8_t k_dummy_model[4] = { 'O', 'N', 'N', 'X' };

alp_inference_config_t base_cfg()
{
	alp_inference_config_t cfg = {};
	cfg.model_data             = k_dummy_model;
	cfg.model_size             = sizeof(k_dummy_model);
	cfg.format                 = ALP_INFERENCE_MODEL_ONNX;
	cfg.backend                = ALP_INFERENCE_BACKEND_CPU;
	return cfg;
}

/* Test 1: a well-formed model whose one input tensor has rank 5 -- over
 * the 4-slot descriptor's kMaxTensorRank, but nowhere near the hostile
 * ceiling.  Refused with NOSUPPORT, not truncated (issue #1729). */
void test_open_refuses_representable_rank_over_4()
{
	ort_test::reset();
	ort_test::g_state.n_in  = 1;
	ort_test::g_state.n_out = 1;
	ort_test::g_state.in_dims.push_back({ 1, 2, 3, 4, 5 });
	ort_test::g_state.out_dims.push_back({ 1, 1 });

	struct alp_inference   h   = {};
	alp_inference_config_t cfg = base_cfg();
	alp_status_t           rc  = alp_inference_ort_open(&h, &cfg);

	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOSUPPORT);
}

/* Test 2: a hostile/corrupt ndim (1000) is a DIFFERENT condition from
 * test 1 -- refused as malformed input (INVAL), not merely
 * "unrepresentable" (NOSUPPORT).  A revision that collapsed the hostile
 * ceiling and the representable-rank ceiling into one check (kMaxTensorRank
 * lowered from 32 straight to 4) reports NOSUPPORT here instead -- this is
 * the test that catches that collapse. */
void test_open_rejects_hostile_rank_as_inval()
{
	ort_test::reset();
	ort_test::g_state.n_in  = 1;
	ort_test::g_state.n_out = 1;
	ort_test::g_state.in_dims.push_back(std::vector<int64_t>(1000, 1));
	ort_test::g_state.out_dims.push_back({ 1, 1 });

	struct alp_inference   h   = {};
	alp_inference_config_t cfg = base_cfg();
	alp_status_t           rc  = alp_inference_ort_open(&h, &cfg);

	ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
}

/* Test 2b: a rank-4 tensor (right at the representable ceiling) opens
 * fine -- no false positive from either guard. */
void test_open_accepts_rank_at_4()
{
	ort_test::reset();
	ort_test::g_state.n_in  = 1;
	ort_test::g_state.n_out = 1;
	ort_test::g_state.in_dims.push_back({ 1, 2, 3, 4 });
	ort_test::g_state.out_dims.push_back({ 1, 1 });

	struct alp_inference   h   = {};
	alp_inference_config_t cfg = base_cfg();
	alp_status_t           rc  = alp_inference_ort_open(&h, &cfg);

	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_TRUE(h.be_state != nullptr);
}

/* Test 3: issue #1494.  n_in=1, n_out=2 -- CreateTensorWithDataAsOrtValue
 * succeeds for the input and for output[0] (2 real OrtValues now exist
 * inside st), then throws on output[1].  Every OrtValue already created
 * must still be released via ~OrtState() when open() unwinds through its
 * catch(...) handler -- not just cpu_mem_info/session/env. */
void test_open_releases_every_ortvalue_on_throw()
{
	ort_test::reset();
	ort_test::g_state.n_in  = 1;
	ort_test::g_state.n_out = 2;
	ort_test::g_state.in_dims.push_back({ 1, 4 });
	ort_test::g_state.out_dims.push_back({ 1, 1 });
	ort_test::g_state.out_dims.push_back({ 1, 1 });
	/* call order: input[0]=0, output[0]=1, output[1]=2 -- throw on the
	 * third CreateTensorWithDataAsOrtValue call, after 2 have succeeded. */
	ort_test::g_state.throw_at_call_index = 2;

	struct alp_inference   h   = {};
	alp_inference_config_t cfg = base_cfg();
	alp_status_t           rc  = alp_inference_ort_open(&h, &cfg);

	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOMEM);
	ALP_ASSERT_EQ_INT(ort_test::g_state.created_value_count, 2);
	ALP_ASSERT_EQ_INT(ort_test::g_state.released_value_count, 2);
	ALP_ASSERT_EQ_INT(ort_test::g_state.released_meminfo_count, 1);
	ALP_ASSERT_EQ_INT(ort_test::g_state.released_session_count, 1);
	ALP_ASSERT_EQ_INT(ort_test::g_state.released_env_count, 1);
}

} /* namespace */

extern "C" const OrtApiBase *OrtGetApiBase(void)
{
	return &g_fake_api_base;
}

int main(void)
{
	test_open_refuses_representable_rank_over_4();
	test_open_rejects_hostile_rank_as_inval();
	test_open_accepts_rank_at_4();
	test_open_releases_every_ortvalue_on_throw();

	ALP_TEST_SUMMARY();
}
