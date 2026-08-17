# `tan model check` + `doctor` — Partition-Screen Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

> **STATUS (post-execution, 2026-08-15).** Every checkbox below still reads
> `- [ ]` because this is the maintainer-approved body, kept byte-identical to
> the scratchpad version it was reviewed against — **not** because the work is
> outstanding. **Tasks 1-6 already shipped in tan-cli**, on branch
> `feat/model-engine-migration` (23 commits ahead of `origin/dev`, not yet
> pushed/PR'd as of this writing):
> - **Tasks 1-3** (the format gate, table resolution, the operator walk + MAC
>   weighting) — `609719d` "feat(model): add the static NPU-eligibility screen
>   engine (ADR-0028 amendment)", merged via `25443c4`. `tan.model.analyze`'s
>   `analyze_backend` has existed since `609719d`; Task 1 Step 2's "run,
>   confirm it fails (`analyze_backend` undefined)" no longer describes this
>   tree — the failing-test step is historical, not a next action.
> - **Task 5** (`tan model doctor`) — `fa05748` "feat(model): add tan model
>   doctor -- NPU compiler toolchain availability", plus follow-ups `b671595`
>   and `6b4f6ec`, merged via `9138d51`.
> - **Task 4** (the partition report + `tan model check` CLI) and **Task 6**
>   (`--exact`) — both in `7522807` "feat(model): wire tan model check -- the
>   static NPU-eligibility screen CLI (tan-cli#782)".
>
> Commit-time ordering against *this* alp-sdk commit: Tasks 1-3 and 5 landed
> in tan-cli **before** this commit (`25443c4` at 2026-08-15 23:03:26 UTC and
> `9138d51` earlier same day, both ahead of this commit's 23:37:21 UTC). Task
> 4/6's `7522807` landed at 23:44:49 UTC — **7 minutes after**, not before —
> so at the moment this alp-sdk commit was made, the engine (Tasks 1-3, 5) was
> already in tan-cli but the CLI wiring (Task 4, 6) was not yet. Re-verify
> against `git -C <tan-cli-checkout> log --oneline` before trusting any of the
> above as still the tan-cli tip.

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
already has `is_available()` — `cpu.py:11-12`, `deepx.py:52`, `drpai.py:138`,
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

> **CORRECTION (post-execution, 2026-08-15).** The paragraph below, as first
> written, was imprecise about *where* the coupling lives and, once corrected
> for that, drew the wrong conclusion. Checked directly against the real tree:
>
> `2026-07-24-vscode-models-panel.md` itself has zero occurrences of
> `fits`/`cpu-fallback`/`no-fit`/`verdict` — it's an abstract plan, not code.
> `alp-sdk-vscode/src/` (the extension host) also has no occurrences of the
> three retired enum strings; the bare word "verdict" does appear there
> (`src/ideHub/messages.ts:220`, `src/lsp/buildConfig.ts:158,513`,
> `src/models/panel.ts:158`), but it names an unrelated field —
> `ModelPrepResultMessage.accuracy.verdict`, the `tan model prep` accuracy
> report, not the NPU fit check. And the host-side fit-check plumbing —
> `checkFit()` shelling `tan model check --board board.yaml`
> (`src/models/panel.ts:166-168`), `ModelFitDataMessage`
> (`src/ideHub/messages.ts:189-201`), `toModelFitData`
> (`src/models/service.ts:108-122`) — is genuinely thin and needs no change:
> `models` stays `unknown[]` at that boundary, a pass-through of whatever
> `tan model check`'s `{sku, models:[{name,source,backends}]}` shape
> (`model_cmd.py:400-401,443` in tan-cli) already emits.
>
> **But that is not the whole surface, and the extension is NOT already
> insulated.** `packages/alp-webview/src/features/models/` — the React view
> this same Plan-C document commissions, per its own Global Constraints
> ("the message protocol is mirrored MANUALLY in both `src/ideHub/messages.ts`
> and `packages/alp-webview/src/types.ts`") — is where `ModelFitDataMessage`'s
> `unknown[]` actually gets narrowed, and it hard-codes exactly the retired
> shape:
> - `useModels.ts:33` — `BackendFit.verdict: "fits" | "cpu-fallback" |
>   "no-fit" | string`.
> - `useModels.ts:34-39` — `BackendFit`'s other fields (`est_sram_kib`,
>   `budget_sram_kib`, `est_latency_ms`, `op_coverage_pct`,
>   `unsupported_ops`) match the *old* `2026-07-24-alp-model-check.md` estimator
>   shape, not the new `BackendReport` wire shape
>   (`backend`/`variant`/`table`/`npuCoverage`/`computeOnNpuPctMax`/
>   `uncostedCpuOpCount`/`basis`/`confidence`/`notes`/`ops`,
>   `python/tan/core/model_check.py:37-54` in tan-cli).
> - `ModelsView.tsx:33-46` — `FIT_SEVERITY`/`FIT_VARIANT`/`FIT_LABEL` are keyed
>   literally on `fits`/`cpu-fallback`/`no-fit`; `fitSeverity()` (`:52-53`)
>   falls back to severity 1 ("warn") for anything not in that map. Once `tan
>   model check` ships the new vocabulary (`npu-eligible`/`cpu-certain`/
>   `unknown` per op, `full-eligible`/`partial`/`cpu-only`/`undetermined` per
>   backend), every badge silently renders "warn" with the raw string as its
>   label — misleading, not crashing, which is the worse failure mode.
>
> So the rename DOES have a lockstep consumer change owed in `alp-sdk-vscode`
> — not in the thin host plumbing, but in
> `packages/alp-webview/src/features/models/useModels.ts` (retype `BackendFit`/
> `ModelFit` to the `BackendReport` shape) and `ModelsView.tsx` (rebuild the
> severity/variant/label maps and the rendered fields around
> `npuCoverage`/`computeOnNpuPctMax`/`ops`). This should be checked, and that
> pair of files updated, before Task 4 lands — the original open question's
> instinct was right; only its assessment of where to look was incomplete.

The **VS Code extension** consumes `tan model *` envelopes via
`packages/alp-webview/src/features/models/` (see the correction above for the
exact files and shape delta this rename owes).

## Deliberately NOT in this plan

- static latency estimates (any backend)
- activation-SRAM as a negative verdict
- a DEEPX op table
- ONNX operator extraction (follow-on; ONNX backends report `undetermined` here)
- the `mera_drp.*` Relay names (incomparable vocabulary, licensed-venv internals)
