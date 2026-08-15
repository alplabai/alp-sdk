# `tan model check` + `doctor` — Partition-Screen Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Answer, offline and with no NPU toolchain installed, *"how much of this
model can target the NPU on this SoM, and what definitely cannot?"* — with claims
the evidence actually supports.

**Supersedes** `docs/superpowers/plans/2026-07-24-alp-model-check.md`, whose
`fits | cpu-fallback | no-fit` vocabulary and TFLite-only scope were both retired
by ADR-0028's 2026-08-15 amendment. That plan's Task 2 (the metadata) already
shipped, in a different shape.

**Architecture:** `tan/model/analyze.py` — a pure engine — loads a support table
keyed by `(family, variant, toolchain)`, gates on the adapter's
`accepts(src_format)` *before* scoring anything, walks the model's operators, and
returns a partition report whose negatives are certified and whose positives are
capped at "eligible". `tan/commands/model_cmd.py` gains `check` and `doctor`
subcommands over it.

## Global Constraints

- **Never emit `fits` from a static screen.** The word is reserved for
  `basis: compiled` or `basis: bench`. ADR-0028 amendment §3.
- **Negatives are sound; positives are bounded.** An operator absent from the
  variant's table is *certainly* CPU. An operator present is *eligible* — never
  "will run on the NPU". Vela attaches Generic constraints (quantization,
  per-axis quant, dtype, zero-point, shape) to every operator and Specific ones
  to 30 of 70; DRP-AI gates on enumerated kernel × stride × padding × dilation ×
  groups. Shape decides, and a static screen cannot see the decision.
- **The failure mode is silent CPU fallback, not refusal.** Every backend
  degrades rather than erroring. The output must say this in words, or the
  customer will read a coverage number as a guarantee.
- **A missing table means `undetermined`, never `cpu-only`.** DEEPX ships no
  table by decision. Conflating "no data" with "no support" would report false
  negatives on V2M, where DEEPX is the headline feature.
- **No static latency in tier 1.** DRP-AI latency depends on an opaque
  partitioner, DEEPX on nothing knowable, and Ethos-U static cycle math ignores
  cascading and memory placement. Latency comes from a real compile or the bench.
- **Heuristic activation-SRAM must never produce a negative verdict.** Vela
  cascades and tiles, so naive peak-activation math *overestimates* — a
  "conservative" bias here manufactures false no-fits, which lose a sale. Only
  weights-vs-storage is a sound static negative.
- Gate: `python -m pytest tests -q` from `python/`, zero failures, in an
  **isolated venv** (a stale `alp-tan` editable install on the maintainer box
  injects ~706 phantom failures). Reference green at time of writing: 4290 passed.
- No Claude/AI attribution. "Alp Lab", never "ALP Lab". Attribute to alpCaner.

---

## Data model

```python
@dataclass(frozen=True)
class OpVerdict:
    op: str                  # as spelled in the model's own vocabulary
    status: str              # "npu-eligible" | "cpu-certain" | "unknown"
    reason: str              # "op-not-in-table" | "constraint-unchecked"
                             # | "no-table-for-backend" | "format-not-accepted"
    macs: int                # 0 when not computable

@dataclass(frozen=True)
class BackendReport:
    backend: str             # cpu | ethos_u | drpai | deepx_dxm1
    variant: str | None      # u85 | u55 | u65 | None
    table: str | None        # the table file that answered, or None
    npu_coverage: str        # "full-eligible" | "partial" | "cpu-only" | "undetermined"
    compute_on_npu_pct_max: float | None   # MAC-weighted UPPER bound
    ops: list[OpVerdict]
    basis: str               # "static-screen" (always, in this slice)
    confidence: str          # "certain" | "screening"
    notes: list[str]
```

`storage: ok | overflow` (weights vs the SoM's flash/MRAM) is reported at model
level, separately — it is the one *sound* static negative.

---

## Task 1: the format gate — refuse before scoring

**Files:** create `python/tan/model/analyze.py`; test
`python/tests/model/test_analyze.py`

The single most important behaviour, and the one the old plan got wrong: a
`.tflite` model scored against DRP-AI or DEEPX is a **category error**, not a
low-confidence answer. Every adapter already declares its ingest format —
`adapters/ethos_u.py` and `adapters/cpu.py` return `src_format == "tflite"`;
`adapters/drpai.py` and `adapters/deepx.py` return `src_format == "onnx"`.

- [ ] **Step 1: failing test**

```python
def test_a_tflite_model_is_not_scored_against_an_onnx_backend():
    rep = analyze_backend(backend="drpai", src_format="tflite", ops=["CONV_2D"],
                          metadata_root=_META)
    assert rep.npu_coverage == "undetermined"
    assert rep.ops[0].reason == "format-not-accepted"
    assert rep.compute_on_npu_pct_max is None
    # and it must NOT claim cpu-only, which would read as "won't run"
    assert rep.npu_coverage != "cpu-only"
```

- [ ] **Step 2:** run, confirm it fails (`analyze_backend` undefined).
- [ ] **Step 3:** implement the gate — ask the adapter's `accepts()`, and short
      circuit to `undetermined` + `format-not-accepted` before any table load.
- [ ] **Step 4:** run, confirm pass.
- [ ] **Step 5:** commit.

## Task 2: table resolution by (family, variant, toolchain)

**Files:** modify `analyze.py`; test `test_analyze.py`

Tables live in alp-sdk at `metadata/npu_ops/<family>/<variant>@<toolchain>-<ver>.json`.
Resolve the SoM's `ethos_u_variant` from its preset, then select the table whose
`applies_to` covers it.

- [ ] **Step 1: failing tests** — `E1M-AEN801` (u85) resolves the 70-op table;
      `E1M-AEN301` (u55) and `E1M-NX9101` (u65) both resolve the 53-op table; a
      backend with no table (`deepx_dxm1`) returns `undetermined` with
      `no-table-for-backend`, and **not** `cpu-only`.
- [ ] **Step 2-4:** implement, verify.
- [ ] **Step 5:** a test that adding a `metadata/npu_ops/deepx*/` table would
      change the DEEPX answer — proving the absence is load-bearing, not
      incidental.
- [ ] **Step 6:** commit.

## Task 3: the operator walk + MAC weighting

**Files:** modify `python/tan/model/tensorio.py` (add `extract_ops`); test
`python/tests/model/test_tensorio.py`

`extract_ops(source, *, raw=None) -> list[OpDesc]` mirroring `extract_io`'s
best-effort contract: `[]` for a non-`.tflite` source, a missing `tflite` reader,
or unparseable bytes — never raises. Carry `TensorDesc(shape, dtype, nbytes,
is_const)` so MACs are computable from static shapes.

MAC weighting matters because **op-count coverage is misleading** — a conv
backbone is nearly all the cost, so "23 of 25 ops eligible" can describe a model
that is 4% eligible by compute.

- [ ] Steps: failing test on the committed `tiny_int8.tflite` fixture → implement
      → verify → commit. ONNX operator extraction is a follow-on; in this slice a
      `.onnx` source returns `[]` and the ONNX backends therefore report
      `undetermined`, honestly.

## Task 4: the partition report + `tan model check`

**Files:** modify `analyze.py`, `python/tan/commands/model_cmd.py`; tests in
`test_analyze.py` and `python/tests/commands/test_model_command.py`

- [ ] The engine returns `BackendReport` per resolved backend.
- [ ] `compute_on_npu_pct_max` is MAC-weighted and **explicitly an upper bound**;
      `None` when MACs are not computable.
- [ ] Human output must state the failure mode in words. Target shape:

```
Ethos-U55 (E1M-AEN501)  partial
  96% of compute (23/25 ops) is NPU-eligible   [upper bound, static screen]
  2 ops are certain CPU fallback: NORMALIZE, TOPK
  Eligible ops carry quantization and shape constraints this check cannot
  verify. The model will RUN either way — unsupported ops fall back to the
  CPU silently rather than failing. Only a real compile proves NPU execution.
  Exact:  pip install ethos-u-vela  &&  tan model check --exact
```

- [ ] `--format json` carries every field with its `basis` and `confidence`.
- [ ] A test asserting the string `"fits"` appears nowhere in any
      `basis: static-screen` output — the cheapest guard against regression to
      the retired vocabulary.

## Task 5: `tan model doctor`

**Files:** modify `model_cmd.py`; test `test_model_command.py`

After ADR-0028 tan is the customer's only diagnostic surface, so a missing or
license-gated compiler must be legible rather than a silent skip. Every adapter
already has `is_available()` — `cpu.py:13`, `deepx.py:52`, `drpai.py:138`,
`ethos_u.py:57`, `executorch.py:36` — and `ethos_u`'s is a bare
`shutil.which("vela") is not None`, so the probe is read-only and non-spawning.

- [ ] Report per backend: `{backend, tool, available, version, reason}`.
      `reason` non-null only when unavailable, and actionable —
      `"vela not on PATH; pip install ethos-u-vela"`,
      `"dxcom not on PATH; license-gated, Linux-only"`.
- [ ] `_vela_version()` degrades to `"vela"` when the distribution is absent
      (`ethos_u.py:21-25`) — surface that as a version of `None`, not a failure.
- [ ] A missing Python or broken SDK root is a first-class `reason`, not a crash.

## Task 6: opportunistic exactness (`--exact`)

Vela is a free, un-gated `pip install`, so for Ethos-U the "offline" constraint
is soft. `check --exact` (or auto-detect `vela` on PATH) runs the real compiler
and returns `basis: compiled` — which is the only basis allowed to say `fits`,
and the only one that yields a true arena figure and cycle estimate.

- [ ] Detect, run, parse; fall back cleanly to the static screen when absent.
- [ ] For DRP-AI/DEEPX this stays opportunistic — both are license-gated.

---

## Open question for the maintainer

The **VS Code extension** consumes `tan model *` envelopes. `2026-07-24-vscode-models-panel.md`
was written against the retired vocabulary. If the extension already models
`fits | cpu-fallback | no-fit`, the rename has a lockstep consumer change in
`alp-sdk-vscode`. That surface was not assessed in this session and should be
checked before Task 4 lands.

## Deliberately NOT in this plan

- static latency estimates (any backend)
- activation-SRAM as a negative verdict
- a DEEPX op table
- ONNX operator extraction (follow-on; ONNX backends report `undetermined` here)
- the `mera_drp.*` Relay names (incomparable vocabulary, licensed-venv internals)
