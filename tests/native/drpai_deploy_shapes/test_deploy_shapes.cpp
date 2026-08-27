/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hermetic native unit test for src/yocto/drpai_deploy_shapes.h -- the
 * hand-rolled deploy.json (TVM graph-runtime JSON) parser backing the
 * Renesas DRP-AI inference backend (issue #1635).  Builds and runs with
 * a plain host g++: no Zephyr, no RUHMI/DRP-AI TVM sysroot, no DRP-AI
 * hardware -- the parser was split out of inference_drpai.cpp into its
 * own vendor-independent header specifically so this is possible (see
 * that header's file comment).  changelog.d/1635.md claims the parser
 * is "verified ... via a standalone unit test"; this is that test.
 *
 * Build + run directly:
 *   g++ -std=c++17 -Wall -Wextra -I src/yocto \
 *       tests/native/drpai_deploy_shapes/test_deploy_shapes.cpp \
 *       -o /tmp/test_deploy_shapes && /tmp/test_deploy_shapes
 * or via tests/scripts/test_drpai_deploy_shapes_native.py, which the
 * normal `pytest tests/scripts/` sweep picks up automatically (skips
 * cleanly if no host C++ compiler is on PATH).
 */
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <unistd.h> /* mkdtemp() */

#include "drpai_deploy_shapes.h"

using namespace alp_drpai;

namespace
{

/** mkdtemp() a private scratch directory.  No untrusted input reaches
 *  the template, so this is safe to shell out to `rm -rf` on later. */
std::string make_scratch_dir()
{
	char        tmpl[] = "/tmp/alp-drpai-test-XXXXXX";
	const char *dir    = ::mkdtemp(tmpl);
	assert(dir != nullptr);
	return std::string(dir);
}

void remove_scratch_dir(const std::string &dir)
{
	std::string cmd = "rm -rf '" + dir + "'";
	(void)std::system(cmd.c_str());
}

/** Write @p contents to `<dir>/deploy.json`. */
void write_deploy_json(const std::string &dir, const std::string &contents)
{
	std::string path = dir + "/deploy.json";
	FILE       *f    = std::fopen(path.c_str(), "wb");
	assert(f != nullptr);
	std::fwrite(contents.data(), 1, contents.size(), f);
	std::fclose(f);
}

/** The real TVM graph shape of a compiled `yolox-s-voc` DRP-AI model:
 *  one placeholder input ("images", node 0) feeding one `tvm_op` node
 *  (node 1) with three detection-head outputs.  `node_row_ptr` [0, 1, 4]
 *  says node 0 occupies shape row 0 (one output, the placeholder rule
 *  `_drpai_parse_deploy_shapes()` checks for) and node 1 occupies rows
 *  1..3 (three outputs); `heads` [[1,0,0],[1,1,0],[1,2,0]] resolves to
 *  those same three rows in order.  Hand-authored (not the vendor
 *  compiler's own deploy.json -- that artifact isn't republished here),
 *  but the node/row_ptr/heads shape and every dimension below (input
 *  `[1,3,640,640]`; outputs `[1,25,80,80]`/`[1,25,40,40]`/`[1,25,20,20]`)
 *  were verified byte-for-byte against a real compiled yolox-s-voc
 *  `deploy.json` on a RUHMI-equipped host before being copied in here --
 *  the same fixture changelog.d/1635.md's DRP-AI section cites. */
const char *kGoodDeployJson = R"JSON(
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

/* Same graph, but the "aux" input's row is a rank-5 shape -- a
 * legitimate (if unusual) tensor shape, not corruption. */
const char *kRank5DeployJson = R"JSON(
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

/** #1: the real yolox-s-voc deploy.json shape -- names, input shape and
 *  all three output shapes resolve exactly as the real compiled model's
 *  graph JSON does. */
void test_real_deploy_json_shape()
{
	std::string dir = make_scratch_dir();
	write_deploy_json(dir, kGoodDeployJson);

	DeployShapes ds;
	bool         ok = _drpai_parse_deploy_shapes(dir, ds);
	assert(ok);
	assert(ds.input_names.size() == 1);
	assert(ds.input_names[0] == "images");
	assert(ds.input_shapes.size() == 1);
	assert((ds.input_shapes[0] == std::vector<uint16_t>{ 1, 3, 640, 640 }));
	assert(ds.output_shapes.size() == 3);
	assert((ds.output_shapes[0] == std::vector<uint16_t>{ 1, 25, 80, 80 }));
	assert((ds.output_shapes[1] == std::vector<uint16_t>{ 1, 25, 40, 40 }));
	assert((ds.output_shapes[2] == std::vector<uint16_t>{ 1, 25, 20, 20 }));

	remove_scratch_dir(dir);
	std::printf("test_real_deploy_json_shape: PASS\n");
}

/** #2: a rank-5 tensor must yield rank 0 (an empty shape entry, so the
 *  caller reports rank == 0) -- NOT a silent truncation to its first 4
 *  dims, which is the issue #1729 defect this backend explicitly
 *  avoids repeating. */
void test_rank5_tensor_yields_rank0_not_truncation()
{
	std::string dir = make_scratch_dir();
	write_deploy_json(dir, kRank5DeployJson);

	DeployShapes ds;
	bool         ok = _drpai_parse_deploy_shapes(dir, ds);
	assert(ok); /* the file itself is well-formed -- rank > 4 isn't corruption */
	assert(ds.input_names.size() == 2);
	assert(ds.input_names[1] == "weird5d");
	assert(ds.input_shapes[1].empty()); /* rank 0 -- unresolved, not {1,3,4,5} */
	/* The OTHER input, unaffected, still resolves normally. */
	assert((ds.input_shapes[0] == std::vector<uint16_t>{ 1, 3, 640, 640 }));

	remove_scratch_dir(dir);
	std::printf("test_rank5_tensor_yields_rank0_not_truncation: PASS\n");
}

/** #3: a malformed/truncated file is rejected outright -- the parser
 *  never guesses at a partially-parsed structure. */
void test_malformed_truncated_file_rejected()
{
	std::string dir = make_scratch_dir();
	std::string truncated(kGoodDeployJson);
	truncated.resize(truncated.size() / 2); /* cut mid-object, mid-token */
	write_deploy_json(dir, truncated);

	DeployShapes ds;
	bool         ok = _drpai_parse_deploy_shapes(dir, ds);
	assert(!ok);

	remove_scratch_dir(dir);
	std::printf("test_malformed_truncated_file_rejected: PASS\n");
}

/** #4: a missing deploy.json is rejected, not treated as "empty shapes
 *  for everything resolved successfully". */
void test_missing_file_rejected()
{
	std::string dir = make_scratch_dir(); /* deploy.json deliberately never written */

	DeployShapes ds;
	bool         ok = _drpai_parse_deploy_shapes(dir, ds);
	assert(!ok);

	remove_scratch_dir(dir);
	std::printf("test_missing_file_rejected: PASS\n");
}

/** #5: correlate_input_shapes() -- the mixed-match case (issue #1635
 *  review finding).  deploy.json names ["a","b"]; the runtime reports
 *  ["b","c"].  "b" matches by name; "c" does not match anything.  The
 *  buggy version fell back to positional assignment PER TENSOR, so "c"
 *  took ds.input_shapes[1] -- "b"'s own shape, already claimed by name
 *  at index 0.  The fix must leave "c" unresolved (empty) instead of
 *  guessing wrong. */
void test_correlate_input_shapes_mixed_match_no_wrong_shape()
{
	DeployShapes ds;
	ds.input_names  = { "a", "b" };
	ds.input_shapes = { { 1, 2 }, { 3, 4 } };

	std::vector<std::string>           runtime_names = { "b", "c" };
	std::vector<std::vector<uint16_t>> out(2);
	correlate_input_shapes(runtime_names, ds, out);

	assert((out[0] == std::vector<uint16_t>{ 3, 4 })); /* "b" matched by name */
	assert(out[1].empty()); /* "c": no name match -- must stay unresolved */

	std::printf("test_correlate_input_shapes_mixed_match_no_wrong_shape: PASS\n");
}

/** #6: correlate_input_shapes() -- when NO input matches by name (and
 *  the counts agree), the positional (graph-input order) fallback DOES
 *  fire for every input -- this is the case the fallback exists for. */
void test_correlate_input_shapes_no_match_uses_positional_fallback()
{
	DeployShapes ds;
	ds.input_names  = { "a", "b" };
	ds.input_shapes = { { 1, 2 }, { 3, 4 } };

	std::vector<std::string>           runtime_names = { "x", "y" }; /* neither matches */
	std::vector<std::vector<uint16_t>> out(2);
	correlate_input_shapes(runtime_names, ds, out);

	assert((out[0] == std::vector<uint16_t>{ 1, 2 }));
	assert((out[1] == std::vector<uint16_t>{ 3, 4 }));

	std::printf("test_correlate_input_shapes_no_match_uses_positional_fallback: PASS\n");
}

} /* namespace */

int main()
{
	test_real_deploy_json_shape();
	test_rank5_tensor_yields_rank0_not_truncation();
	test_malformed_truncated_file_rejected();
	test_missing_file_rejected();
	test_correlate_input_shapes_mixed_match_no_wrong_shape();
	test_correlate_input_shapes_no_match_uses_positional_fallback();
	std::printf("ALL TESTS PASSED\n");
	return 0;
}
