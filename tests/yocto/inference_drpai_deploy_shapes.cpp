/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for the hand-rolled deploy.json (TVM
 * graph-runtime JSON) parser backing the Renesas DRP-AI inference
 * backend (issue #1635): src/yocto/drpai_deploy_shapes.h.  That parser
 * was split out of src/yocto/inference_drpai.cpp into its own header
 * specifically because it has ZERO dependency on
 * MeraDrpRuntimeWrapper.h / <linux/drpai.h> / the RUHMI DRP-AI TVM
 * sysroot the rest of that file needs to compile -- so, unlike
 * inference_drpai.cpp itself, it links here with no vendor stack and
 * no DRP-AI hardware.  changelog.d/1635.md's DRP-AI section claims the
 * parser is "verified via a standalone unit test"; this is that test.
 *
 * Deliberately NOT linked against alp::sdk: drpai_deploy_shapes.h pulls
 * in nothing from the SDK (only <cstdint>/<cstdio>/<string>/<vector>),
 * so there is nothing to link and no ODR risk to dodge either way.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_inference_drpai_deploy_shapes
 *   ctest --test-dir build -R alp_test_inference_drpai_deploy_shapes
 */

#include <unistd.h> /* mkdtemp() */

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "test_assert.h"

#include "../../src/yocto/drpai_deploy_shapes.h"

using namespace alp_drpai;

/* ------------------------------------------------------------------ */
/* Fixture helpers.                                                    */
/* ------------------------------------------------------------------ */

/** mkdtemp() a private scratch directory.  No untrusted input reaches
 *  the template, so this is safe to shell out to `rm -rf` on later. */
static std::string make_scratch_dir(void)
{
	char        tmpl[] = "/tmp/alp-drpai-test-XXXXXX";
	const char *dir    = ::mkdtemp(tmpl);
	ALP_ASSERT_TRUE(dir != NULL);
	return std::string(dir);
}

static void remove_scratch_dir(const std::string &dir)
{
	std::string cmd = "rm -rf '" + dir + "'";
	(void)std::system(cmd.c_str());
}

/** Write @p contents to `<dir>/deploy.json`. */
static void write_deploy_json(const std::string &dir, const std::string &contents)
{
	std::string path = dir + "/deploy.json";
	FILE       *f    = std::fopen(path.c_str(), "wb");
	ALP_ASSERT_TRUE(f != NULL);
	std::fwrite(contents.data(), 1, contents.size(), f);
	std::fclose(f);
}

/** The real TVM graph shape of a compiled `yolox-s-voc` DRP-AI model:
 *  one placeholder input ("images", node 0) feeding one `tvm_op` node
 *  (node 1) with three detection-head outputs.  `node_row_ptr` [0, 1, 4]
 *  says node 0 occupies shape row 0 (one output, the placeholder rule
 *  drpai_deploy_shapes.h checks for) and node 1 occupies rows 1..3
 *  (three outputs); `heads` [[1,0,0],[1,1,0],[1,2,0]] resolves to those
 *  same three rows in order.  Hand-authored (not the vendor compiler's
 *  own deploy.json -- that artifact isn't republished here), but the
 *  node/row_ptr/heads shape and every dimension below (input
 *  `[1,3,640,640]`; outputs `[1,25,80,80]`/`[1,25,40,40]`/`[1,25,20,20]`)
 *  were verified byte-for-byte against a real compiled yolox-s-voc
 *  `deploy.json` on a RUHMI-equipped host before being copied in here --
 *  the same fixture changelog.d/1635.md's DRP-AI section cites. */
static const char *kGoodDeployJson = R"JSON(
{
  "nodes": [
    {"op": "null", "name": "images", "inputs": []},
    {"op": "tvm_op", "name": "tvmgen_default_mera_drp_main_0", "inputs": [[0, 0, 0]],
     "attrs": {"func_name": "tvmgen_default_mera_drp_main_0", "num_inputs": "1", "num_outputs": "3"}}
  ],
  "arg_nodes": [0],
  "heads": [[1, 0, 0], [1, 1, 0], [1, 2, 0]],
  "node_row_ptr": [0, 1, 4],
  "attrs": {
    "shape": ["list_shape", [[1, 3, 640, 640], [1, 25, 80, 80], [1, 25, 40, 40], [1, 25, 20, 20]]],
    "dltype": ["list_str", ["float32", "float16", "float16", "float16"]]
  }
}
)JSON";

/* Same graph, but the second input's row is a rank-5 shape -- a
 * legitimate (if unusual) tensor shape, not corruption. */
static const char *kRank5DeployJson = R"JSON(
{
  "nodes": [
    {"op": "null", "name": "images", "inputs": []},
    {"op": "null", "name": "weird5d", "inputs": []},
    {"op": "tvm_op", "name": "fused_conv", "inputs": [[0, 0, 0], [1, 0, 0]],
     "attrs": {"func_name": "fused_conv", "num_inputs": "2", "num_outputs": "1"}}
  ],
  "arg_nodes": [0, 1],
  "heads": [[2, 0, 0]],
  "node_row_ptr": [0, 1, 2, 3],
  "attrs": {
    "shape": ["list_shape", [[1, 3, 640, 640], [1, 3, 4, 5, 6], [1, 1000]]],
    "dltype": ["list_str", ["float32", "float32", "float32"]]
  }
}
)JSON";

/* ------------------------------------------------------------------ */
/* Tests.                                                               */
/* ------------------------------------------------------------------ */

/** #1: the real yolox-s-voc deploy.json shape -- names, input shape and
 *  all three output shapes resolve exactly as the real compiled model's
 *  graph JSON does. */
static void test_real_deploy_json_shape(void)
{
	std::string dir = make_scratch_dir();
	write_deploy_json(dir, kGoodDeployJson);

	DeployShapes ds;
	bool         ok = _drpai_parse_deploy_shapes(dir, ds);
	ALP_ASSERT_TRUE(ok);
	ALP_ASSERT_EQ_INT(ds.input_names.size(), 1);
	ALP_ASSERT_TRUE(ds.input_names[0] == "images");
	ALP_ASSERT_EQ_INT(ds.input_shapes.size(), 1);
	ALP_ASSERT_TRUE((ds.input_shapes[0] == std::vector<uint16_t>{ 1, 3, 640, 640 }));
	ALP_ASSERT_EQ_INT(ds.output_shapes.size(), 3);
	ALP_ASSERT_TRUE((ds.output_shapes[0] == std::vector<uint16_t>{ 1, 25, 80, 80 }));
	ALP_ASSERT_TRUE((ds.output_shapes[1] == std::vector<uint16_t>{ 1, 25, 40, 40 }));
	ALP_ASSERT_TRUE((ds.output_shapes[2] == std::vector<uint16_t>{ 1, 25, 20, 20 }));

	remove_scratch_dir(dir);
}

/** #2: a rank-5 tensor must yield rank 0 (an empty shape entry, so the
 *  caller reports rank == 0) -- NOT a silent truncation to its first 4
 *  dims, which is the issue #1729 defect this backend explicitly
 *  avoids repeating. */
static void test_rank5_tensor_yields_rank0_not_truncation(void)
{
	std::string dir = make_scratch_dir();
	write_deploy_json(dir, kRank5DeployJson);

	DeployShapes ds;
	bool         ok = _drpai_parse_deploy_shapes(dir, ds);
	ALP_ASSERT_TRUE(ok); /* the file itself is well-formed -- rank > 4 isn't corruption */
	ALP_ASSERT_EQ_INT(ds.input_names.size(), 2);
	ALP_ASSERT_TRUE(ds.input_names[1] == "weird5d");
	ALP_ASSERT_TRUE(ds.input_shapes[1].empty()); /* rank 0 -- unresolved, not {1,3,4,5} */
	/* The OTHER input, unaffected, still resolves normally. */
	ALP_ASSERT_TRUE((ds.input_shapes[0] == std::vector<uint16_t>{ 1, 3, 640, 640 }));

	remove_scratch_dir(dir);
}

/** #3: a malformed/truncated file is rejected outright -- the parser
 *  never guesses at a partially-parsed structure. */
static void test_malformed_truncated_file_rejected(void)
{
	std::string dir = make_scratch_dir();
	std::string truncated(kGoodDeployJson);
	truncated.resize(truncated.size() / 2); /* cut mid-object, mid-token */
	write_deploy_json(dir, truncated);

	DeployShapes ds;
	bool         ok = _drpai_parse_deploy_shapes(dir, ds);
	ALP_ASSERT_TRUE(!ok);

	remove_scratch_dir(dir);
}

/** #4: a missing deploy.json is rejected, not treated as "empty shapes
 *  for everything resolved successfully". */
static void test_missing_file_rejected(void)
{
	std::string dir = make_scratch_dir(); /* deploy.json deliberately never written */

	DeployShapes ds;
	bool         ok = _drpai_parse_deploy_shapes(dir, ds);
	ALP_ASSERT_TRUE(!ok);

	remove_scratch_dir(dir);
}

/** #5: correlate_input_shapes() -- the mixed-match case (issue #1635
 *  review finding).  deploy.json names ["a","b"]; the runtime reports
 *  ["b","c"].  "b" matches by name; "c" does not match anything.  The
 *  buggy version fell back to positional assignment PER TENSOR, so "c"
 *  took ds.input_shapes[1] -- "b"'s own shape, already claimed by name
 *  at index 0.  The fix must leave "c" unresolved (empty) instead of
 *  guessing wrong. */
static void test_correlate_input_shapes_mixed_match_no_wrong_shape(void)
{
	DeployShapes ds;
	ds.input_names  = { "a", "b" };
	ds.input_shapes = { { 1, 2 }, { 3, 4 } };

	std::vector<std::string>           runtime_names = { "b", "c" };
	std::vector<std::vector<uint16_t>> out(2);
	correlate_input_shapes(runtime_names, ds, out);

	ALP_ASSERT_TRUE((out[0] == std::vector<uint16_t>{ 3, 4 })); /* "b" matched by name */
	ALP_ASSERT_TRUE(out[1].empty()); /* "c": no name match -- must stay unresolved */
}

/** #6: correlate_input_shapes() -- when NO input matches by name (and
 *  the counts agree), the positional (graph-input order) fallback DOES
 *  fire for every input -- this is the case the fallback exists for. */
static void test_correlate_input_shapes_no_match_uses_positional_fallback(void)
{
	DeployShapes ds;
	ds.input_names  = { "a", "b" };
	ds.input_shapes = { { 1, 2 }, { 3, 4 } };

	std::vector<std::string>           runtime_names = { "x", "y" }; /* neither matches */
	std::vector<std::vector<uint16_t>> out(2);
	correlate_input_shapes(runtime_names, ds, out);

	ALP_ASSERT_TRUE((out[0] == std::vector<uint16_t>{ 1, 2 }));
	ALP_ASSERT_TRUE((out[1] == std::vector<uint16_t>{ 3, 4 }));
}

int main(void)
{
	test_real_deploy_json_shape();
	test_rank5_tensor_yields_rank0_not_truncation();
	test_malformed_truncated_file_rejected();
	test_missing_file_rejected();
	test_correlate_input_shapes_mixed_match_no_wrong_shape();
	test_correlate_input_shapes_no_match_uses_positional_fallback();

	ALP_TEST_SUMMARY();
}
