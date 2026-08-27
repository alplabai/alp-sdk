### Fixed — DRP-AI backend reported `rank=0` for every tensor (#1635)

`alp_inference_drpai_get_input()`/`get_output()` set `rank = 0` with an empty
`shape[]`, because `MeraDrpRuntimeWrapper::GetInputInfo()`/`GetOutputInfo()`
only report `(name, size_bytes, dtype)` — no shape. That comment was true but
irrelevant: the shape is on disk the whole time. `deploy.json`, the TVM
graph-runtime JSON `scripts/alp_model/adapters/drpai.py` tars into every
`drpai_dir` blob alongside `drp_desc.bin`/`weight.bin`, carries every graph
input's and output's shape, and lands flat in `st->model_dir` before
`open()` returns.

Both hooks now resolve real rank/shape from that file. `open()` parses
`deploy.json` once (a new file-local, dependency-free JSON scanner — alp-sdk
carries no JSON library, so this understands only the narrow TVM graph-JSON
subset it needs: nodes, `node_row_ptr`, `arg_nodes`, `heads`, and the
`attrs.shape` "list_shape" table) and correlates each entry to the existing
`in_info`/`out_info` vectors: inputs by node name (falling back to
graph-input positional order only when the counts agree), outputs
positionally (`deploy.json`'s `heads` order already matches `GetOutput(idx)`
order). Anything that doesn't resolve cleanly — the file missing or
unreadable, a schema that doesn't match, a corrupt dim — leaves that
tensor's `rank` at 0, identical to the prior behaviour; a wrong shape is
worse than no shape, so the parser bails rather than guesses.

A tensor whose real rank exceeds 4 also keeps `rank = 0`, deliberately.
`alp_inference_tensor_t.shape` is a frozen `uint16_t[4]`
(`[ABI-STABLE]`, `include/alp/inference.h`), and the sibling ORT/DEEPX
backends silently truncate a rank > 4 tensor to its first 4 dims — now
filed as #1729. DRP-AI does not add a third behaviour to that question.

Verified against the real `deploy.json` from a compiled `yolox-s-voc`
DRP-AI model (input `images` → `[1,3,640,640]`; three detection-head
outputs → `[1,25,80,80]`/`[1,25,40,40]`/`[1,25,20,20]`) plus synthetic
malformed inputs (missing file, truncated/non-object JSON, a non-integer
dim, an out-of-range node index, and a rank-5 shape) via a standalone unit
test, and by syntax-checking the modified translation unit against a
minimal local stub of the vendor `MeraDrpRuntimeWrapper.h`/`linux/drpai.h`
surface. **Code-complete and bench-unverified**: the DRP-AI runtime only
executes on V2N silicon under the MERA2/DRP-AI TVM sysroot, which does not
exist on this dev host — this change has not run on real hardware.
