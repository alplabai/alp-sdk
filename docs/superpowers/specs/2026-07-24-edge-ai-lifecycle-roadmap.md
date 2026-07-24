# Edge-AI model lifecycle — value-add roadmap + sub-project 1 (pre-flight fit/perf)

- **Date:** 2026-07-24
- **Status:** Design (brainstormed + approved; program vision + sub-project-1 detail)
- **Builds on:** the model & edge-AI management pipeline (`docs/superpowers/specs/2026-07-24-model-edge-ai-management-design.md` + Plans A/B/C — declare → compile → package `.alpmodel` → inspect, shipped as PRs alp-sdk #907, tan-cli #47, vscode #310). This roadmap adds the value-adds *around* that pipeline to make edge-AI development easy + smooth.
- **Repos:** alp-sdk (analyzer + data + `alp model check`), tan-cli (`tan model check`), alp-sdk-vscode (fit badge). Same 3-layer shape as the pipeline.

## 1. Vision — the edge-AI model lifecycle platform

Alp becomes the one place a customer goes from *"I have/need a model"* to *"it's running + measured on my SoM"* — where the unified, portable `.alpmodel` (one model, auto-selected NPU blob across Alif / Renesas / NXP) means **no vendor lock-in and no per-NPU expertise required**.

The moat vs vendor-locked tooling (Arm `vela`, DEEPX `dxcom`, Renesas DRP-AI TVM — each single-vendor, license-gated): **portability + manufacturer-owned fit/perf data**. As the SoM manufacturer we know our silicon best; the op-support lists and bench-measured performance points we author are data no competitor can produce for our modules, and they power the whole lifecycle.

## 2. Program decomposition (4 sub-projects, cheap → heavy)

Each is its own spec → plan → build cycle. Ordered by dependency + cost.

| # | Sub-project | What | Depends on |
|---|---|---|---|
| **1** | **Pre-flight fit/perf** *(this spec)* | `tan model check` — "will this model fit my SoM's NPU, and how fast?" without a compile/flash. Green/yellow/red fit badge before build. | The coverage-matrix + `requires`/`ALP_SOC_NPU_ARENA_SRAM_KIB` the pipeline already emits |
| **2** | **Model zoo** | Curated per-SoM-validated starters (person-detect, keyword-spotting, anomaly-detection, VAD) with pre-computed fit/perf; `tan model add <zoo>` → one-command working model | #1's fit/perf per SoM |
| **3** | **Prep wizard** | Guided TFLite/PyTorch → ONNX + INT8 quantize + calibration-set management + per-backend config (dxcom JSON / DRP-AI spec) scaffolded | The pipeline; the license-gated toolchains |
| **4** | **Measure + iterate** | Phase-3 run/observe + on-device telemetry (latency, backend-ran, confidence, power via bench PSU) → dashboard; **A/B two models** to pick the best per SoM | Deploy infra (bench-gated Yocto runtimes); validates #1's estimates |

**Why #1 first:** cheapest (analysis over data we already emit), highest-leverage (every other piece consumes its fit/perf verdict), and it kills the worst loop today — *compile → flash → discover it doesn't fit or is too slow.*

## 3. Sub-project 1 — pre-flight fit/perf analyzer

### 3.1 Three fidelity tiers (best-available wins)

Locked decision (SoM-manufacturer CX call): **hybrid, and the manufacturer pre-computes the authoritative numbers.** A customer usually lacks the license-gated NPU toolchains, and the moment fit matters most is evaluation — so the check must work with zero local toolchain, yet never give a confidently-wrong "fits."

1. **Static estimator** — always, offline, any machine, no toolchain. Parse the ONNX/TFLite graph and compute, per SoM backend: op-coverage vs the NPU's supported-op list; peak-SRAM (activation-buffer high-water vs `ALP_SOC_NPU_ARENA_SRAM_KIB`); rough latency (Σ MACs ÷ NPU MAC/cycle × clock). Rough + instant + universal. Every verdict from this tier is labelled `source: static` and carries an "estimate — verify on silicon" caveat.
2. **Manufacturer-precomputed** — we run the real toolchains + silicon on our bench for the model-zoo + common models × every SoM, and **ship exact fit / latency / SRAM / power as authoritative data** in metadata (`metadata/model_perf/…`). Customers get *exact* numbers for known models with no local toolchain. `source: precomputed`.
3. **Exact-on-demand** — when a customer *does* have the toolchain (or via a future cloud compile), run `vela --show-cpu-operations` / `dxcom` analysis / the DRP-AI checker for exact op-partition + arena on their custom model. `source: exact`.

The bench-measured points (tier 2) also **calibrate the static estimator** (tier 1) over time — a per-NPU throughput/overhead model fit to measured data, so static estimates converge toward reality.

### 3.2 What it computes (per SoM backend)

- **verdict**: `fits` (runs entirely on the NPU) / `cpu-fallback` (runs, but N ops fall to CPU → slow) / `no-fit` (won't run — SRAM overflow or a hard-unsupported op → the existing `ALP_ERR_NO_FIT`).
- **est_sram_kib** (peak activation arena) + the SoM's budget, so overflow is visible.
- **est_latency_ms** (ballpark; exact when tier 2/3).
- **op_coverage_pct** + **unsupported_ops[]** (the ops forcing CPU fallback / no-fit).
- **source**: `static` | `precomputed` | `exact` (honesty about fidelity).

### 3.3 Actionable verdicts (incl. the in-family cross-sell)

Every non-`fits` verdict carries a concrete next step:
- `cpu-fallback` → name the unsupported ops + suggest an NPU-friendly substitute or quantization.
- `no-fit` (SRAM) → suggest quantize/prune, **or the next-bigger SoM in our range that does fit** (computed from the same per-SoM data) — a cross-sell that keeps the customer in-family instead of churning to a competitor's part.
- `no-fit` (op) → point at the prep wizard (sub-project 3) or a supported alternative.

### 3.4 The data asset (manufacturer-owned — the moat)

- **Per-NPU supported-op lists** in metadata (Ethos-U from vela's known set; DRP-AI + DEEPX from their docs + bench probing). Powers op-coverage + fit.
- **Per-(SoM × model) bench-measured perf points** (`metadata/model_perf/…`): exact latency/SRAM/power. Authored on our bench (we hold the toolchains + silicon). Feeds tiers 2 and the tier-1 calibration.

These are authored + owned by us; keep them under the public/private split ([[classifying-public-vs-internal]]) — the *contract* (schema) public, raw bench captures where they belong.

### 3.5 Surface (3 layers, mirrors the pipeline)

- **alp-sdk**: `alp model check <model> [--sku SKU]` — the static analyzer + the metadata readers + the tiered resolution (precomputed → exact-if-toolchain → static). Emits the domain payload `{backends:[{backend, verdict, est_sram_kib, budget_sram_kib, est_latency_ms, op_coverage_pct, unsupported_ops, source}], suggestion?}`.
- **tan-cli**: `tan model check` — envelope-wraps it (same pattern as `tan model {build,list,info,doctor}`).
- **extension**: a green/yellow/red **fit badge per SoM backend** in the Models panel, shown **before build**; the check runs automatically as `tan model build`'s pre-flight (fail-fast on a doomed compile), on model-declare, and on-demand. Click → the detail report (SRAM bar, op-coverage, unsupported ops, the suggestion).

### 3.6 Risks / honest caveats

- **Static-estimate accuracy** — latency especially is bandwidth/scheduling-sensitive; tier-1 numbers are ballparks and MUST be labelled as estimates. Fit (SRAM/op-support) is more reliable than latency. The tier-2 calibration is what earns trust over time.
- **Op-support-list authoring effort** — real work per NPU, and it drifts with toolchain versions. Version the lists; regenerate from `vela`/`dxcom`/DRP-AI where scriptable.
- **False "fits" is the worst failure** — bias the static estimator conservative (round buffers up, treat unknown ops as CPU-fallback) so it never over-promises. A wrong "fits" churns a customer.
- **Not a substitute for tier-3/silicon** — the check informs; the pipeline + measure loop (sub-project 4) confirm.

## 4. Sub-projects 2–4 (sketches — spec'd when reached)

- **2 · Model zoo (designed 2026-07-24).** Killer CX: *browse models that run on YOUR module, one-click add, guaranteed to fit.* **Licensing decision — link + fetch + layer, do NOT redistribute weights:** a zoo entry LINKS the upstream source (ONNX Model Zoo / TF Hub / Ultralytics) and we add the SoM value on top — per-SoM validation, the pre-computed fit/perf (from #1), a known-good compile config (the fiddly dxcom calibration / DRP-AI spec), and a ready example. Plus a few **own-trained, clean-license reference models** for the flagship tasks (person-detect exists) so there is always a guaranteed-clean "it just works" starter; we ship the *SoM knowledge*, not other people's weights. v1 seeds ~5 edge-AI greatest-hits mapping to our NPUs: person-detection (vision), image-classification (MobileNet), keyword-spotting (small audio), anomaly-detection (vibration — the example exists), VAD. Entry schema `metadata/model_zoo/<id>.yaml`: `{id, task, description, source (url+sha | bundled), license, io_spec, validated_soms, perf_ref (→ #1 data), compile (per-backend config), example_app}`. Surface: `alp/tan model add <zoo-id>` → appends to `board.yaml` `models[]` + fetches/caches the source (+ optional example scaffold); extension **gallery filtered to "runs on your SoM"** (via #1's fit verdict), each card = task + fit badge + one-click Add. Moat: the curation + per-SoM fit data (same manufacturer-owned asset as #1). Schema public; any licensed/private weights per [[classifying-public-vs-internal]].
- **3 · Prep wizard (designed 2026-07-24).** The step where most edge-AI projects stall. **Call 1 — prep is LICENSE-FREE:** convert (TFLite/PyTorch/Keras → ONNX) + INT8 quantize + the accuracy check all run on free cross-platform tools (`onnxruntime`, `onnx`) — NO dxcom/DRP-AI needed; the vendor-specific compile stays the separate (license-gated) pipeline step. Any customer preps on any machine; the license only bites at final compile (which we pre-compute/cloud). **Call 2 — accuracy preservation IS the value:** INT8 silently tanks accuracy and no one notices until on-device, so after quantize we run **fp32 vs int8 on the calibration set → an accuracy-delta report**, and when it's bad we guide the fix (per-channel quant, more/better calibration data, keep sensitive ops fp16). Plus calibration-set validation (min N samples, shape match, distribution sanity — a bad calibration set is the silent accuracy killer). Turns "quantization is a dark art" into a guided, MEASURED flow — the differentiator. Surface: `alp/tan model prep <raw> --calibration <dir> [--sku]` → quantized ONNX + per-backend compile config (dxcom JSON / DRP-AI spec, sane defaults) + accuracy report, wired into `board.yaml` `compile:`; extension wizard (point model + calibration → convert → quantize → accuracy report → add-to-project, which flows into #1 fit-check + build). Moat: the per-NPU sane defaults + accuracy-preservation guidance — manufacturer expertise codified.
- **4 · Measure + iterate** — the deferred Phase-3 run/observe loop + telemetry (latency/backend-ran/confidence/power) → dashboard + A/B compare; validates #1's estimates against real silicon. Bench-gated on the Yocto NPU runtimes.

## 5. Success criteria (sub-project 1)

- `alp model check <model> --sku <SKU>` returns, offline with no NPU toolchain, a per-backend fit verdict + est SRAM/latency/op-coverage from the static estimator, labelled `source: static`.
- For a model-zoo/known model, the check returns `source: precomputed` exact numbers with no local toolchain.
- A model that overflows a SoM's arena reports `no-fit` + suggests the next-bigger in-family SoM that fits.
- The extension shows the fit badge before build and auto-pre-flights `tan model build`.
