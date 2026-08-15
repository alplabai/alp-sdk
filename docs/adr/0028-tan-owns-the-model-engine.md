# 0028. Tan owns the model engine; alp-sdk keeps the NPU truth

Status: Proposed
Date: 2026-08-15 (Caner)
Deciders: alpCaner (alp-sdk, tan-cli)
Pairs with: [0026](0026-tan-owns-the-planner-outright.md) — this ADR applies
0026's hardware-truth-vs-consumer principle to the second duplicated surface.
Amends: [0020](0020-sdk-owns-build-execution.md) only insofar as `alp model`
stops being an SDK-side verb; the single-executor decision is unchanged.
Supersedes the pipeline-spine clause (§2 item 4) of
`docs/superpowers/specs/2026-07-24-model-edge-ai-management-design.md` — see
*Why the 2026-07-24 decision is reversed* below.

## Amendment (2026-08-15 — what a static `check` may claim, and how the tables are keyed)

Decision-2 keeps `metadata/npu_ops/` in alp-sdk as hardware truth. Deriving that
data from the real toolchains changed both its **shape** and what a consumer may
honestly say with it. Recorded here because
`docs/superpowers/plans/2026-07-24-alp-model-check.md` is superseded on both
counts.

**1. Keyed by support-table identity, not by backend.**

```
metadata/npu_ops/
  ethos_u/u85@vela-5.1.0.json          70 ops · tflite · tool-generated
  ethos_u/u55-u65@vela-5.1.0.json      53 ops · tflite · tool-generated
  drpai/onnx-i8@translator-1.12.json   47 ops · onnx   · vendor-manual
  (no deepx/ — deliberately)
```

Vela 5.1.0 publishes **two** TFLite tables: Ethos-U85 (70 operators) and
Ethos-U55/U65 (53). U55/U65 is a strict subset; exactly 17 are U85-only —
`CAST`, `DIV`, `EQUAL`, `GATHER`, `GREATER`, `GREATER_EQUAL`, `LESS`,
`LESS_EQUAL`, `LOGICAL_AND`, `LOGICAL_NOT`, `LOGICAL_OR`, `NOT_EQUAL`,
`REDUCE_ALL`, `REDUCE_ANY`, `SCATTER_ND`, `SELECT`, `SELECT_V2`. alp-sdk ships
all three variants (u85: E1M-AEN401/601/801; u55: E1M-AEN301/501/701; u65:
E1M-NX9101), so one flat `ethos_u.json` carrying 70 would report 17 false
capabilities on four of the seven Ethos-U SKUs. The tables are invariant to
`--accelerator-config` — five regenerations across `ethos-u55-32`,
`ethos-u55-256`, `ethos-u65-512`, `ethos-u85-128` and `ethos-u85-2048` produced
byte-identical output — so the architecture family, not the MAC count, is the
real key.

**2. DEEPX ships no table, and the absence is the signal.** dxcom 2.3.0
publishes no op-support list; the only provable claim is the 15 ONNX operators
present in a `yolo11n.onnx` that dxcom demonstrably compiled — a lower bound.
Shipping it would mass-produce false negatives on V2M, the SKU where DEEPX is
the headline feature. A consumer finding no table MUST return `undetermined`,
never degrade to "all ops unknown → cpu-fallback".

**3. A static check screens; it never certifies a fit.** Every table carries
`stance: "screening"`, and the `fits | cpu-fallback | no-fit` vocabulary is
retired. On all three backends the real failure mode is **silent CPU fallback**,
not refusal:

- Vela marks no operator unconditionally supported — every entry carries Generic
  constraints (quantization, per-axis quant, dtype, zero-point, shape) and 30 of
  the 70 carry Specific ones; a non-conforming instance falls back to CPU rather
  than failing the compile.
- DRP-AI TVM partitions unmatched ops to `llvm -device=arm_cpu`; a *matched*
  subgraph is still pushed back if it lacks a compute-intensive op; and
  acceptance gates on enumerated kernel × stride × padding × dilation × groups,
  so the same operator name is accepted or rejected on tensor shape alone.
- DRP-AI additionally has a second, opaque MERA2 partitioner whose criteria are
  unavailable.

A name-based verdict is therefore an **upper bound**. The replacement is a
partition screen: per-op `npu-eligible | cpu-certain | unknown`; model-level
`compute_on_npu_pct_max` (MAC-weighted, explicitly an upper bound) and
`npu_coverage: full-eligible | partial | cpu-only | undetermined`;
`storage: ok | overflow` as the one *sound* static negative; every field
labelled `basis: static-screen | compiled | bench`. **"fits" is reserved for
`basis: compiled` or `basis: bench`.** A consumer must gate on the adapter's
`accepts(src_format)` before scoring operators at all — Vela ingests TFLite
while DRP-AI TVM and dxcom ingest ONNX, so scoring a TFLite artifact against an
ONNX backend's table is a category error, not a lower-fidelity answer.

**4. `drpai`'s list is the Translator's, deliberately.** DRP-AI TVM's own gate
is a Relay composite-pattern table (`mera_drp.conv2d`, `mera_drp.gemm`, …),
which is incomparable to a customer's ONNX model without a mapping layer that
does not exist, and which lives inside a licensed venv that must not be
redistributed here. TVM provably invokes the DRP-AI Translator as its backend
codegen (`relay.ext.drp.set_toolchain`), and `rzv_drp-ai_tvm/README.md:30` calls
the stack "an extension of the DRP-AI Translator to the TVM backend" — so the
Translator's published ONNX list is the right screening data, pinned to
`drp_compiler_version: "100"` and the int8 flow.

## Context

`alp model` is the second surface that ships from two repos at once, and it is
at the stage the planner was in before its duplication metastasised. Measured
at alp-sdk `40bfc917` and tan-cli `57bb2fa1`:

| Thing | Size |
|---|---|
| alp-sdk `scripts/alp_model/` (the engine) | 13 modules, **1,029 lines** |
| alp-sdk `scripts/alp_cli/model.py` (the CLI) | **66 lines**, one verb (`build`) |
| tan `python/tan/commands/model_cmd.py` | **607 lines**, one verb (`build`) |
| of which: hand-ported SDK-domain logic | **108 lines** |
| of which: `python -c` driver + subprocess plumbing | **129 lines** |
| unlanded verbs waiting in alp-sdk#933 | **9** (`check`/`list`/`doctor`/`info`/`run`/`ab`/`prep`/`zoo`/`add`) |

`model_cmd.py` is neither a port nor a forward. It is a hybrid: it hand-ports
the *surface* (board.yaml discovery, `som.sku` and `models[]` validation,
compile-option path resolution, the summary) and forwards the *engine* through
a 30-line `python -c` driver string that imports `alp_model.build` under the
SDK's own interpreter with `PYTHONPATH=<sdk>/scripts`. Its own module docstring
claims the opposite — "This is a REAL implementation, not a forward" — which is
true of the surface and false of the engine.

### The duplication has already produced a shipped defect

alp-sdk#1271 restricted compile-option path resolution to the four keys that
actually name paths:

```python
_PATH_OPT_KEYS = {"config", "calibration", "images", "spec"}
```

because resolving every value "corrupted a genuine shape string into a
filesystem path, which then made the adapter's own shape check misfire"
(`scripts/alp_cli/model.py:19-21`). tan's hand-ported counterpart
(`model_cmd.py:128-140`) never received that fix and still resolves **every**
string option. So `tan model build` currently path-mangles DRP-AI's
`input_shape` (`"1,3,224,224"`), `input_name` (`"images"`) and `product`
(`"V2N"`). The hash pin did not catch it: a pin proves the *upstream* file is
unchanged, never that the *downstream* port is equivalent.

### The pin regime's measured cost

`python/tests/gates/test_planner_relocation_freshness.py` is **987 lines**
carrying three independent pins (`PINNED_SDK_COMMIT` and
`HAND_PORT_PINNED_SDK_COMMIT` both `bd8be484680cf5aa1c1ac0e8b38d84128b5a279d`,
`STRICT_LOADERS_PINNED_SDK_COMMIT` `26b0040e9a762c16aff5c7c53b2e19cc7583b2a4`)
over 21 + 19 + 1 pinned sha256s. `scripts/alp_cli/model.py` is one of them, at
`a51be0a8d3a16bd408bb57d01f049175406b73cc48ab9346d39555c3aa5b1925`.

18 commits touched that gate file in the 16 days 2026-07-30 → 2026-08-14; 13
moved a pin or a hash — roughly one re-pin every 26 hours, hand-carried across
11 dedicated re-sync PRs totalling +8094/-1138 over 161 file-touches. The
automation built to absorb it (`python/scripts/planner_resync.py`, 880 lines)
has opened zero proposal PRs in five days and failed 27 of its 29 runs, and by
design it **never** merges a hand-port: `HAND_PORT_HASHES` is flagged, never
3-way merged, because these counterparts are restructured, renamed or split and
there is no base/ours/theirs triple a merge could be correct over.

Adding nine verbs to that regime multiplies exactly this cost by nine.

## Why the 2026-07-24 decision is reversed

`2026-07-24-model-edge-ai-management-design.md` §2 item 4 locked the opposite
spine — "tan wraps Python; extension stays thin" — and rejected the
alternative in these words: *"re-implementing the pipeline in **Rust**
(duplicates finished work; NPU tools are Python-ecosystem)"*.

Both halves of that rationale have since expired:

1. **tan is not Rust.** The Python port landed at `v0.5.0-rc1` (2026-07-31).
   The Python-ecosystem argument for keeping the engine on the far side of a
   subprocess is gone — tan can `import` it.
2. **Relocating is not re-implementing.** The rejected alternative was writing
   the pipeline a second time. This ADR moves the one implementation, deletes
   the origin, and creates no second copy to keep honest.

The forwarding spine is also no longer *mechanically* reachable. `alp` as an
installable command was deleted in `629aa75f` (2026-07-21, ADR-0020);
`pyproject.toml`'s `[project.scripts]` registers only `alp-mcp`. What survives
is `PYTHONPATH=scripts python3 -m alp_cli model build` from a *source
checkout*, which emits plain text (`built <path>`), has no `--format`, no
envelope, no `schemaVersion`, and reports failure as a raw traceback at exit 1.
It cannot be installed either: the wheel's `include = ["alp_cli*", "alp_mcp*"]`
omits `alp_model`, so a wheel-only `python -m alp_cli model --help` raises
`ModuleNotFoundError: No module named 'alp_model'`.

For the record, the tan side never wanted the hand-port. tan-cli#58's
`crates/tan-cli/src/commands/model.rs` (689 lines) implements zero model logic
— all ten of its verbs spawn `python -m alp_cli model <sub> --format json` and
wrap the child's stdout. The hand-port arrived for an unrelated reason, stated
in the porting commit: ADR-0020 end-state B required alp-sdk to *delete*
`scripts/alp_cli/`, and "deleting `alp_cli` would have removed shipped
functionality rather than relocating it." Relocating the engine is what makes
that deletion possible without loss — it is the completion of ADR-0020, not a
departure from it.

## Decision

**Tan owns the model engine. alp-sdk owns the NPU truth the engine reads.**

1. `python/tan/model/` becomes the single implementation of the host-side model
   pipeline — adapters, `build`, `manifest`, `package`, `targets`, `tensorio`,
   and the analyzers the unlanded verbs add. It sits beside `tan/planner/` and
   follows the same relocation pattern.
2. **alp-sdk keeps what it is uniquely authoritative for**, exactly as
   [0026](0026-tan-owns-the-planner-outright.md) clause 2 states for the
   planner:
   - `metadata/npu_ops/<backend_family>/<variant>@<toolchain>-<toolchain_version>.json`
     — per-NPU op support, one file per SUPPORT-TABLE IDENTITY, not per
     backend (see the Amendment above for why and for the shape's full
     rationale). This is the manufacturer-owned moat, refined from bench
     probing and real-toolchain reports; it is hardware truth and it stays
     in the repo that owns the hardware.
   - `metadata/model_zoo/` and every `metadata/schemas/*.schema.json`.
   - `metadata/socs/**`'s `inference_arena_sram_kib` arena budgets.
   - `scripts/validate_metadata.py`, which gates all of the above.
   - the on-device C: `src/common/alp_model.c`, `src/common/alp_model_loader.c`,
     `src/backends/inference/alp_model_select.c`, `include/alp/model.h`.
3. **The `.alpmodel` container format survives unchanged** as the cross-repo
   seam — tan writes it, alp-sdk's C reads it on the device. Retiring a
   *producer* does not retire a *format*, the same distinction 0026 clause 3
   draws for `build-plan-v1`. It needs a version guard on the same terms.
4. **No parity apparatus is created for this axis.** There is nothing to police
   because there is no second implementation: the engine moves, it is not
   forked. `scripts/alp_cli/model.py` leaves `HAND_PORT_HASHES` when it is
   deleted, and `HAND_PORT_PINNED_SDK_COMMIT` loses one of its entries rather
   than gaining nine.
5. The nine unlanded verbs in alp-sdk#933 are written **once**, in tan. #933's
   Python becomes a port source of the same kind tan-cli#58's Rust already is.

### Migration, in an order that is safe to stop at any point

1. **alp-sdk lands the hardware truth first**, standalone: `metadata/npu_ops/`
   + `metadata/schemas/npu-ops-v1.schema.json` + the `validate_metadata.py`
   block. This is additive, useful on its own, and blocks nothing.
2. **tan relocates the engine** to `python/tan/model/`, repoints the one
   external import, ports the alp-sdk#1271 drift fix, and reshapes
   `model_cmd.py` into argparse + envelope over an in-process call. alp-sdk is
   untouched and still works at this point — the two coexist for exactly one
   slice.
3. **alp-sdk deletes** `scripts/alp_model/` and `scripts/alp_cli/model.py`, with
   `git grep` evidence, and rehomes the three cross-cutting tests that survive
   the move.
4. Verbs land in tan, cheapest-and-most-useful first: `check`, then the
   envelope set, then `zoo`/`add`, `prep`, and `run`/`ab`/`measure` last.

Steps 1 and 2 are independent and may run concurrently. Step 3 must not start
before step 2 is merged, or `tan model build` has no engine.

## Consequences

**Good.** One implementation, so a fix ships once. The alp-sdk#1271 class of
silent port drift stops existing. `scripts/alp_cli/` becomes deletable, closing
ADR-0020's last open thread. Nine verbs cost nine implementations instead of
eighteen plus nine re-audits. The engine gains tan's release train, so a model
fix reaches users through the binary they already have rather than requiring a
matching SDK checkout.

**Bad / accepted.** alp-sdk can no longer compile a model standalone. Unlike
the planner's equivalent cost, **this one is close to free**: no alp-sdk build
step imports the Python `alp_model`. Every CMake, Yocto and example reference
resolves to the C file `src/common/alp_model.c`
(`zephyr/CMakeLists.txt:1392-1394`, `src/baremetal/CMakeLists.txt:111`,
`src/yocto/CMakeLists.txt:107,116,169`, `tests/unit/alpmodel_select/`,
`tests/yocto/`, `meta-alp-sdk/recipes-devtools/zcbor/zcbor_0.9.1.bb:5`), and
`examples/aen/aen-npu-inference-alp/CMakeLists.txt:79` drives its own
`gen_model.py`, which `subprocess`-spawns `vela` directly and never imports
`alp_model`. What is lost is the *Python test suite*, which moves with the
engine, plus three cross-cutting tests that must be rehomed:
`tests/scripts/test_silicon_ref_single_source.py:93,106`,
`tests/scripts/test_alp_cli_new_som.py:335`,
`tests/scripts/test_resolve_generated_conflicts.py:54`.

**Risk.** The two real-model end-to-end tests
(`tests/scripts/test_deepx_yolo_internal.py`,
`tests/scripts/test_vela_yolo_internal.py`) resolve their yolo11n fixtures out
of the private `alp-sdk-internal` checkout. tan has no such wiring today, so the
move must either carry that resolution across or the tests become
alp-sdk-resident integration tests against a tan-provided engine. Deciding this
is step 2's, not an afterthought — losing the real-model proof would violate the
standing "prove capabilities on real models" rule.

## Alternatives considered

- **Keep the hybrid, add nine verbs to it (status quo).** Rejected on the
  measurement above: one re-pin every 26 hours already, `planner_resync.py`
  failing 27 of 29 runs, and a shipped `_resolve_compile` defect the pin could
  not see. Nine more verbs is nine more of each.
- **True runtime forwarding — tan spawns the SDK CLI and only renders the
  envelope.** This was the design's original spine and tan-cli#58 implements it
  cleanly, but it is unreachable as shipped: no `alp` console script since
  `629aa75f`, no `--format`/envelope/`schemaVersion` on the module invocation,
  and a wheel that omits `alp_model` entirely. Making it reachable means
  building an installable, versioned, envelope-emitting SDK CLI — i.e. building
  a second product to avoid moving 1,029 lines. It also re-breaks ADR-0020 by
  keeping `scripts/alp_cli/` permanently undeletable, and reintroduces the
  subprocess boundary the Python port removed, which 0026 rejected for the
  planner on the same grounds.
- **Formalise the `_DRIVER` payload as a versioned contract and keep the split.**
  Worth doing on its merits and cheap, but it does not address this problem: the
  driver seam is not where the drift happened. alp-sdk#1271 drifted in the
  **108 hand-ported lines above** the driver, which no payload schema observes.
  A versioned contract would have shipped the same bug. It is retained as the
  `.alpmodel` format guard in Decision-3, where the seam is real.
- **Extract a separately-versioned `alp-model` pip package both repos consume.**
  Rejected for now: it buys real decoupling at the price of a third release
  train, and there is no second consumer to justify it — alp-sdk stops consuming
  the engine entirely under this decision, so the package would have exactly one
  user. Revisit only if a genuine second consumer appears (a cloud build
  service is the plausible one, per the design's reserved `--remote` seam).
