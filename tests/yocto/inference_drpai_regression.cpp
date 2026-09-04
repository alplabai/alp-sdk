/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression + compile coverage for src/yocto/inference_drpai.cpp
 * (issue #1747: alongside inference_ort.cpp and inference_deepx.cpp,
 * this backend was compiled by NO PR gate -- all three default OFF, so a
 * change to any of them could ship broken straight into a release).
 *
 * The EdgeCortix MERA2 / DRP-AI TVM runtime is Renesas/EdgeCortix
 * account-gated and only exists on the RZ/V Yocto SDK sysroot, not in
 * CI -- same constraint inference_ort_regression.cpp and
 * inference_deepx_regression.cpp already solve for ORT/DEEPX. This test
 * applies the same technique: compile src/yocto/inference_drpai.cpp
 * directly against tests/yocto/fakes/drpai/, a clean-room stand-in for
 * MeraDrpRuntimeWrapper.h (see that file's own comment) plus a fake
 * linux/drpai.h for the DRP-AI driver uapi struct/ioctl it also needs.
 *
 * Coverage differs from the ORT/DEEPX regression tests in one load-
 * bearing way: inference_drpai.cpp's open() resolves the DRP-AI
 * reserved-memory arena via a REAL `::open("/dev/drpai0", O_RDWR)` +
 * `::ioctl(..., DRPAI_GET_DRPAI_AREA, ...)` (see its own
 * "_drpai_mem_start" doc comment) -- a genuine host syscall, not
 * something a header fake can intercept. No CI runner (and no dev
 * host) has /dev/drpai0, so that call always fails with ENOENT and
 * open() always returns ALP_ERR_IO before ever constructing a
 * MeraDrpRuntimeWrapper, regardless of what the fake class does. That
 * is not a gap in this test: "the device is absent" -> ALP_ERR_IO,
 * not a crash or a guess, IS the documented contract
 * (_drpai_errno_to_status's own comment: "ENOENT ... is what lets a
 * caller tell 'no DRP-AI on this board' from 'busy, retry'"), and it is
 * exactly what these tests exercise for real, with no mocking needed.
 * What these tests do NOT reach: LoadModel()/GetInputInfo()/Run() and
 * the tar-staging path, all of which sit behind that same device probe.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_inference_drpai_regression
 *   ctest --test-dir build -R alp_test_inference_drpai_regression
 */

#include <cstddef>
#include <cstdint>

extern "C" {
#include "alp/inference.h"
}

#include "inference_handle_internal.h"
#include "test_assert.h"

/* Forward declarations -- mirrors src/yocto/inference_yocto.c's own
 * #if defined(ALP_SDK_USE_DRPAI_V2N) block; this test links
 * inference_drpai.cpp directly rather than through the dispatcher, so it
 * declares the same hook prototypes itself. */
extern "C" {
alp_status_t alp_inference_drpai_open(struct alp_inference *h, const alp_inference_config_t *cfg);
std::size_t  alp_inference_drpai_num_inputs(struct alp_inference *h);
std::size_t  alp_inference_drpai_num_outputs(struct alp_inference *h);
alp_status_t alp_inference_drpai_get_input(struct alp_inference   *h,
                                           std::size_t             index,
                                           alp_inference_tensor_t *out);
alp_status_t alp_inference_drpai_get_output(struct alp_inference   *h,
                                            std::size_t             index,
                                            alp_inference_tensor_t *out);
alp_status_t alp_inference_drpai_invoke(struct alp_inference *h);
void         alp_inference_drpai_close(struct alp_inference *h);
}

namespace
{

const uint8_t          k_dummy_model[4] = { 'D', 'R', 'P', 'a' };
alp_inference_config_t base_cfg()
{
	alp_inference_config_t cfg = {};
	cfg.model_data             = k_dummy_model;
	cfg.model_size             = sizeof(k_dummy_model);
	cfg.format                 = ALP_INFERENCE_MODEL_DRPAI;
	cfg.backend                = ALP_INFERENCE_BACKEND_DRPAI;
	return cfg;
}

/* Test 1: open() rejects a NULL model_data before touching the device
 * (the earliest guard in the function -- host-independent). */
void test_open_rejects_null_model_data()
{
	struct alp_inference   h   = {};
	alp_inference_config_t cfg = base_cfg();
	cfg.model_data             = nullptr;

	alp_status_t rc = alp_inference_drpai_open(&h, &cfg);

	ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
	ALP_ASSERT_NULL(h.be_state);
}

/* Test 1b: open() rejects a zero model_size the same way. */
void test_open_rejects_zero_model_size()
{
	struct alp_inference   h   = {};
	alp_inference_config_t cfg = base_cfg();
	cfg.model_size             = 0;

	alp_status_t rc = alp_inference_drpai_open(&h, &cfg);

	ALP_ASSERT_EQ_INT(rc, ALP_ERR_INVAL);
	ALP_ASSERT_NULL(h.be_state);
}

/* Test 2: with a well-formed cfg, open() still fails cleanly -- no
 * /dev/drpai0 exists on this host, so _drpai_mem_start() hits ENOENT and
 * open() returns ALP_ERR_IO without allocating be_state.  This is the
 * "no DRP-AI on this board" contract from _drpai_errno_to_status's own
 * doc comment, exercised for real (no fake needed for this path). */
void test_open_fails_cleanly_when_device_absent()
{
	struct alp_inference   h   = {};
	alp_inference_config_t cfg = base_cfg();

	alp_status_t rc = alp_inference_drpai_open(&h, &cfg);

	ALP_ASSERT_EQ_INT(rc, ALP_ERR_IO);
	ALP_ASSERT_NULL(h.be_state);
}

/* Test 3: num_inputs()/num_outputs() on a never-opened handle report 0,
 * not a crash on a NULL be_state. */
void test_num_inputs_outputs_zero_when_not_open()
{
	struct alp_inference h = {};

	ALP_ASSERT_EQ_INT(alp_inference_drpai_num_inputs(&h), 0);
	ALP_ASSERT_EQ_INT(alp_inference_drpai_num_outputs(&h), 0);
}

/* Test 4: get_input()/get_output() on a never-opened handle report
 * ALP_ERR_NOT_READY, not a NULL-deref. */
void test_get_input_output_not_ready_when_not_open()
{
	struct alp_inference   h   = {};
	alp_inference_tensor_t out = {};

	ALP_ASSERT_EQ_INT(alp_inference_drpai_get_input(&h, 0, &out), ALP_ERR_NOT_READY);
	ALP_ASSERT_EQ_INT(alp_inference_drpai_get_output(&h, 0, &out), ALP_ERR_NOT_READY);
}

/* Test 5: invoke() on a never-opened handle reports ALP_ERR_NOT_READY. */
void test_invoke_not_ready_when_not_open()
{
	struct alp_inference h = {};

	ALP_ASSERT_EQ_INT(alp_inference_drpai_invoke(&h), ALP_ERR_NOT_READY);
}

/* Test 6: close() on a never-opened (be_state == NULL) handle is a
 * no-op, not a crash. */
void test_close_on_null_state_is_noop()
{
	struct alp_inference h = {};

	alp_inference_drpai_close(&h);

	ALP_ASSERT_NULL(h.be_state);
}

} /* namespace */

int main(void)
{
	test_open_rejects_null_model_data();
	test_open_rejects_zero_model_size();
	test_open_fails_cleanly_when_device_absent();
	test_num_inputs_outputs_zero_when_not_open();
	test_get_input_output_not_ready_when_not_open();
	test_invoke_not_ready_when_not_open();
	test_close_on_null_state_is_noop();

	ALP_TEST_SUMMARY();
}
