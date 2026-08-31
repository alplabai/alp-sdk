<!-- SPDX-License-Identifier: Apache-2.0 -->
# Capturing a tier-2 model-perf point

How to add one bench-measured performance point to
`metadata/model_perf/` once the three blockers listed below (see
"Blockers -- read this before you spend bench time") clear. Written
**before** any bench time is spent on tier 2
(docs/superpowers/specs/2026-07-24-edge-ai-lifecycle-roadmap.md
sub-project 1), so a first capture campaign follows a recipe instead
of improvising one against the schema.

`metadata/model_perf/` ships **empty** as of issue #1520: this doc
describes the contract a real capture must satisfy, not a procedure
that has been run yet. Do not write an estimated, extrapolated, or
synthetic point into `metadata/model_perf/` to fill the gap — a perf
point comes off real Alp Lab bench silicon or it does not exist. A
worked example for tests lives under
[`tests/fixtures/model_perf/`](../../tests/fixtures/model_perf/), never
under the published tree (`scripts/validate_metadata.py` refuses any
`_fixture`-named path there).

## Blockers — read this before you spend bench time

Three open questions gate the *data*, not the contract landed here.
None is solved by this doc; each is called out so a capture session
doesn't discover it mid-bench.

1. **The vela profile question.** `vela` picks its OWN built-in
   default system/memory profile (`Ethos_U85_SYS_DRAM_Mid` /
   `Dedicated_Sram_384KB`) whenever `--system-config` /
   `--memory-mode` are omitted — a **DRAM-backed** profile, on Alp Lab
   silicon that has **no DRAM**. Every `ethos_u` point's `vela:` block
   is schema-required precisely so a capture can't silently inherit
   that default unrecorded, but *which* profile a module should
   actually compile under is not decided yet. Do not invent one;
   capture whichever profile is in use once this is resolved, and
   record it verbatim in `vela.system_config` / `vela.memory_mode`.
2. **The const-region question.** `perf.req_sram_kib` today counts
   the compiled activation arena alone (the same accounting as the
   `.alpmodel` manifest's `Target.requires.sram_kib`) — it does not
   yet account for the model's const/weight region on backends where
   that region is SRAM- rather than MRAM/flash-resident. A bench point
   captured before this is resolved is recording a real number, just
   not the whole SRAM picture; note that in `capture.notes`.
3. **No timing harness.** No AEN example times an inference, and
   `<alp/inference.h>` exposes no latency accessor — the first capture
   campaign has to lift one (start an inference, stop it, read a
   monotonic timer around the call) before it can fill any
   `perf.latency_ms` field. Until then, a point may legitimately omit
   `perf.latency_ms` entirely and record fit/SRAM alone; the schema
   allows this on purpose (see `model-perf-v1.schema.json`'s
   description on that field).

## What a point is keyed on

A perf point's identity is the full **measurement** — SoM SKU, hw_rev,
compile target (backend + accel_config + core), the exact source-model
bytes, and the vela profile when one applies — not the model alone and
not the SoM alone. Two captures that share a model but differ in any
of those fields are two different measurements and get two different
files; see `metadata/model_perf/README.md` and
`metadata/schemas/model-perf-v1.schema.json` for the full contract.

## Recipe

1. **Resolve the target.** For the SKU under test, run
   `alp model check <model> --sku <SKU>` (or read
   `scripts/alp_model/targets.py`'s `resolve_targets()` directly) to
   get the exact `(backend, accel_config, core)` triples that SKU
   resolves. A perf point naming anything else fails
   `scripts/validate_metadata.py`'s target cross-check.
2. **Compile for that target.**
   - `ethos_u`: run `vela <model>.tflite --accelerator-config
     <accel_config> --system-config <profile> --memory-mode <mode>
     --output-dir <out>` — pass BOTH profile flags explicitly (see
     blocker 1); never rely on vela's own default.
   - Other backends: use that backend's own compile step (`dxcom` /
     the DRP-AI toolchain / `.alpmodel` build via
     `scripts/alp_model/build.py`).
3. **Read `req_sram_kib` / `arena_bytes`** from the compiler's own
   report (vela's `<stem>_summary_*.csv`, or the `.alpmodel` manifest
   `Target.arena` / `Target.requires.sram_kib` if building through
   `alp_model.build`) — do not hand-estimate either value.
4. **Flash + run the timed harness** (once it exists — blocker 3) on
   the real SoM, ≥ 30 back-to-back inferences after any warm-up runs
   are discarded, and compute `mean` / `p50` / `p95` / `stdev` /
   `runs` from that sample. `scripts/validate_metadata.py` refuses
   fewer than 30 runs and refuses a `p95` below `mean`.
5. **Fill `capture`**: `date` (ISO-8601, the day of the run),
   `operator`, `bench_id` (the physical rig, e.g. `e1m-aen-evk-01`),
   and `notes` for anything a reader trusting the number should know
   (thermal state, firmware build, the const-region caveat from
   blocker 2).
6. **Compute the filename** by content-hashing the identity fields —
   `scripts/validate_metadata.py`'s `_model_perf_identity_hash()` is
   the single source of that recipe; do not hand-invent a filename.
7. **Write the file** to `metadata/model_perf/<SKU>/<hash>.yaml` and
   run `py -3 scripts/validate_metadata.py` — it must report `OK` for
   the new file with no semantic-check failures before it's committed.

## Reference

- Schema: [`metadata/schemas/model-perf-v1.schema.json`](../../metadata/schemas/model-perf-v1.schema.json).
- Semantic checks: `scripts/validate_metadata.py`'s
  `_check_model_perf_semantics()` (SKU existence, target/core
  resolution, hw_rev family membership, the vela-profile requirement,
  the SRAM/latency sanity checks, the run-count floor, the `_fixture`
  refusal).
- Test coverage for every rule above: `tests/scripts/test_model_perf_metadata.py`.
- Consumer: `tan model check`'s tiered resolution
  (precomputed → exact-if-toolchain → static) reads these points to
  emit `basis: "bench"` — tracked in `alplabai/tan-cli`, out of scope
  here.
