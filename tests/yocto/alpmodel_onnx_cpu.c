/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #1254 end-to-end regression: an "onnx" blob_format .alpmodel
 * container, decoded by the REAL zcbor-backed alp_model_parse()
 * (src/common/alp_model.c) now that src/yocto/CMakeLists.txt compiles it
 * unconditionally instead of the ALP_ERR_NOSUPPORT stub
 * (src/common/stub/stub_model.c, dropped from that file's source list in
 * the same change), selects ALP_INFERENCE_BACKEND_CPU through
 * alp_model_select() -- the ONNX Runtime CPU backend's actual entry
 * point (PR #1263).
 *
 * The fixture bytes (k_onnx_cpu_alpmodel, tests/yocto/onnx_cpu_fixture.h)
 * are the real zcbor wire format, not a hand-typed guess: GENERATED with
 * the SAME canonical writer scripts/alp_model/package.py uses for
 * tests/unit/alpmodel_reader's own fixture.h (Manifest/Target ->
 * write_package(), via scripts/alp_model/_gen_fixture.py's
 * build_onnx_cpu_fixture_bytes()). tests/unit/alpmodel_select/ already
 * covers the SELECTOR against hand-built alp_model_t structs (no CBOR
 * involved); this test is the one place in the suite that exercises the
 * REAL zcbor decode this issue restores.
 *
 * Regenerate with (from the repo root):
 *   python3 -m alp_model._gen_fixture
 * tests/scripts/test_alp_model_package.py's
 * test_committed_onnx_cpu_fixture_matches_generator asserts the
 * committed header stays in sync with the generator -- a container-format
 * change that forgets to regenerate this file fails that test.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_alpmodel_onnx_cpu
 *   ctest --test-dir build -R alp_test_alpmodel_onnx_cpu
 */

#include <stdint.h>
#include <string.h>

#include "alp/inference.h"
#include "alp/model.h"

#include "../../src/backends/inference/alp_model_select.h"
#include "onnx_cpu_fixture.h"
#include "test_assert.h"

static void test_parse_decodes_the_real_cbor_manifest(void)
{
	alp_model_t  m;
	alp_status_t rc = alp_model_parse(k_onnx_cpu_alpmodel, k_onnx_cpu_alpmodel_len, &m);
	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_TRUE(strcmp(m.name, "onnx_cpu_smoke") == 0);
	ALP_ASSERT_EQ_INT(m.n_targets, 1u);
	ALP_ASSERT_TRUE(strcmp(m.targets[0].backend, "cpu") == 0);
	ALP_ASSERT_TRUE(strcmp(m.targets[0].blob_format, "onnx") == 0);
	ALP_ASSERT_EQ_INT(m.targets[0].blob_len, 30u);
}

static void test_onnx_target_selects_cpu_backend(void)
{
	alp_model_t m;
	ALP_ASSERT_EQ_INT(alp_model_parse(k_onnx_cpu_alpmodel, k_onnx_cpu_alpmodel_len, &m), ALP_OK);

	/* Minimal env: silicon_ref "*" (the CPU wildcard) is always
	 * available regardless of soc_ref/avail_silicon -- see
	 * alp_model_select.c's _silicon_available(). arena_sram_kib=0 means
	 * "unknown budget", which always fits (the manifest's own
	 * requires.sram_kib is 0 here too) -- and, per #1731, the win is
	 * flagged r.arena_fit_unverified rather than looking like a real
	 * fit check ran. */
	alp_model_select_env_t    env = { .preferred_backend = ALP_INFERENCE_BACKEND_AUTO };
	alp_model_select_result_t r   = { 0 };
	ALP_ASSERT_EQ_INT(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_AUTO, &r), ALP_OK);
	ALP_ASSERT_EQ_INT(r.backend, ALP_INFERENCE_BACKEND_CPU);
	ALP_ASSERT_EQ_INT(r.target_index, 0u);
	ALP_ASSERT_TRUE(r.arena_fit_unverified);
}

int main(void)
{
	test_parse_decodes_the_real_cbor_manifest();
	test_onnx_target_selects_cpu_backend();

	ALP_TEST_SUMMARY();
}
