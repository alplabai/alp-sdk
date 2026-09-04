# Manufacturer-Precomputed Model Perf (Tier 2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Let a customer with **no NPU toolchain and no silicon** get exact,
bench-measured fit/latency/SRAM for a known model on their SoM — the middle
fidelity tier that `docs/superpowers/specs/2026-07-24-edge-ai-lifecycle-roadmap.md`
§3.1 calls the manufacturer-owned moat.

**Architecture:** alp-sdk publishes bench-measured perf points as identity-keyed
metadata under `metadata/model_perf/`, exactly as it already publishes op-support
tables under `metadata/npu_ops/`. `tan model check` gains a **tiered resolution**
— precomputed → exact-if-toolchain → static — and emits the `basis: "bench"` its
vocabulary already reserves but has never produced.

**Tech Stack:** JSON metadata + Draft 2020-12 schema (alp-sdk), Python 3.12
(`python/tan/model/`), `ethos-u-vela` 5.1.0 / `dxcom` / DRP-AI TVM on the bench.

## Why this tier, and why now

Tiers 1 and 3 shipped in `2026-08-16-vela-memory-profile.md` and its
predecessors: the static screen always works offline, and `--exact` runs the real
compiler when the customer happens to hold it. The middle tier is missing, and it
is the one that matters commercially — at evaluation time a prospect has neither
the license-gated toolchain nor the board, which is precisely when "will this run
on your module?" decides the sale.

`metadata/model_perf/` does not exist today (verified). `basis: "bench"` is
reserved at `python/tan/model/analyze.py:25-26` and produced by nothing.

The spec also names a second consumer, and it is the reason the *shape* matters
more than the first batch of data (§3.1): *"The bench-measured points (tier 2)
also **calibrate the static estimator** (tier 1) over time — a per-NPU
throughput/overhead model fit to measured data, so static estimates converge
toward reality."* A perf point is therefore a durable measurement record, not a
cache of one answer.

## Global Constraints

- **Never author a perf number that was not measured.** Every field in a perf
  point comes from a real run on real silicon with a named toolchain. There is no
  "estimated" perf point — that is tier 1's job, and conflating them destroys the
  only thing tier 2 sells. If a figure was not measured, omit the field; if the
  whole point was not measured, do not create the file.
- **A perf point is pinned to what produced it, or it is a lie.** It carries the
  model's `sha256`, the SoM SKU, the accel config, the toolchain **and its
  version**, and the silicon revision. If any of those move, the point no longer
  applies and the consumer must fall through to the next tier rather than
  reporting a stale exact number as authoritative. **This is the load-bearing
  gate** — without it tier 2 becomes another check that cannot fail, which this
  programme has now produced at five separate layers.
- **An absent perf point is `undetermined`, never a negative.** Same rule the
  `npu-ops-v1` schema states for a missing op table: absence means "no data", not
  "does not fit". A consumer that reads absence as a bad verdict reports false
  negatives on every model we have not yet benched, which is most of them.
- **Public/private split** (spec §3.4, and `[[classifying-public-vs-internal]]`):
  the **schema and the perf points are public**; raw bench captures (serial logs,
  PSU traces, unreleased-silicon detail) stay in `alp-sdk-internal`. A perf point
  cites its capture; it does not embed it.
- **`basis` is the vocabulary, not `source`.** The spec §3.2 wrote
  `source: static | precomputed | exact`; the shipped envelope uses
  `basis: "static-screen" | "compiled" | "bench"` with `confidence`. Do NOT
  introduce a second parallel field — map `precomputed` onto `basis: "bench"` and
  record the mapping in the ADR/plan rather than in a new key.
- **`fits` remains reserved.** Only `basis: "compiled"` or `basis: "bench"` may
  emit it. Tier 2 is the second surface ever permitted to, so the existing guard
  (`python/tests/model/test_check.py`, bound to the live template) must be
  extended, not bypassed.
- alp-sdk gate: `bash scripts/test-all.sh --target dev` — exit 2 is a SKIP, not a
  failure; `twister` SKIPs locally without `ZEPHYR_BASE`, so that run is never a
  complete gate. tan gate: the BARE shape from `python/`, zero failures.
- alp-sdk changelog fragments are `changelog.d/<issue>.md`, DIGITS ONLY; tan-cli
  uses `<issue>.<kind>.md`. No AI attribution. "Alp Lab", never "ALP Lab".

## File Structure

**alp-sdk** (the facts + the contract):
- Create `metadata/schemas/model-perf-v1.schema.json` — the perf-point contract.
- Create `metadata/model_perf/<sku>/<model-slug>@<toolchain>-<version>.json` —
  one file per measurement identity.
- Modify `scripts/validate_metadata.py` — register the family, plus the semantic
  cross-checks a schema cannot express.
- Create `tests/scripts/test_model_perf_metadata.py`.
- Create `docs/bench/model-perf-capture.md` — the recipe the bench must follow to
  produce a point, so a measurement is reproducible by someone else.

**tan-cli** (the consumer):
- Modify `python/tan/model/perf.py` (new) — load + match perf points.
- Modify `python/tan/model/check.py` — tiered resolution.
- Modify `python/tan/model/analyze.py` — `BackendReport` carries the bench fields.
- Tests in `python/tests/model/test_perf.py`, `test_check.py`.

---

## Task 1: the perf-point contract (alp-sdk)

**Files:**
- Create: `metadata/schemas/model-perf-v1.schema.json`
- Test: `tests/scripts/test_model_perf_metadata.py`

**Interfaces:**
- Produces: the schema every perf point validates against, consumed by Task 2's
  validator and Task 4's tan reader.

Follow `metadata/schemas/npu-ops-v1.schema.json` — it already solves the same
problems (identity-keyed filename, an explicit `stance`, an absence rule stated
in the schema's own `description`).

Shape, keyed by MEASUREMENT identity rather than by model:

```json
{
  "measured_on": {
    "sku": "E1M-AEN801",
    "hw_rev": "aen801 r2",
    "accel_config": "ethos-u85-256",
    "backend": "ethos_u"
  },
  "model": {
    "slug": "person-detect-int8",
    "sha256": "808cfdfc0cf3a6fa6f6fa26bfa379ea97c16d5db7334637766e39c3408502e9d",
    "size_bytes": 300568,
    "source": "tests/fixtures/models/person_detect_int8.tflite"
  },
  "toolchain": { "name": "vela", "version": "5.1.0", "profile": "Ethos_U85_SRAM_Only" },
  "measured": {
    "npu_ops": 44, "cpu_ops": 0,
    "arena_bytes": 74480, "req_sram_kib": 73,
    "latency_ms_mean": 12.4, "latency_ms_p95": 12.9, "runs": 100
  },
  "capture": {
    "date": "2026-08-16",
    "operator": "alpCaner",
    "reference": "alp-sdk-internal:bench/captures/2026-08-16-aen801-person-detect.log"
  }
}
```

- [ ] **Step 1: failing test** — `tests/scripts/test_model_perf_metadata.py`:

```python
import json
from pathlib import Path

_META = Path(__file__).resolve().parents[2] / "metadata"
_PERF = _META / "model_perf"


def _perf_files():
    return sorted(_PERF.glob("**/*.json")) if _PERF.is_dir() else []


def test_every_perf_point_pins_the_model_by_hash():
    # A point that does not name the exact bytes it measured cannot be
    # invalidated when the model changes, and a stale exact number is worse
    # than no number: the customer trusts it precisely because it says "bench".
    for p in _perf_files():
        d = json.loads(p.read_text(encoding="utf-8"))
        sha = d.get("model", {}).get("sha256", "")
        assert len(sha) == 64 and all(c in "0123456789abcdef" for c in sha), (
            f"{p.name}: model.sha256 must be a full lowercase sha256"
        )


def test_every_perf_point_pins_its_toolchain_version():
    for p in _perf_files():
        d = json.loads(p.read_text(encoding="utf-8"))
        tc = d.get("toolchain", {})
        assert tc.get("name") and tc.get("version"), (
            f"{p.name}: a measurement is only valid for the toolchain that produced it"
        )


def test_no_perf_point_claims_an_unmeasured_latency():
    # `runs` is what separates a measurement from a guess.
    for p in _perf_files():
        d = json.loads(p.read_text(encoding="utf-8"))
        m = d.get("measured", {})
        if "latency_ms_mean" in m:
            assert m.get("runs", 0) >= 1, (
                f"{p.name}: latency without a run count is not a measurement"
            )
```

- [ ] **Step 2:** run — expect the tests to pass **vacuously** (no perf files
      yet). That is the trap this step exists to expose: write one fixture point
      under `tests/fixtures/model_perf/` and assert the checks bite on it, so the
      suite is not green purely because the directory is empty.
- [ ] **Step 3:** write the schema; wire it in Task 2.
- [ ] **Step 4:** run, verify.
- [ ] **Step 5:** commit. Fragment `changelog.d/<issue>.md`.

## Task 2: validator + the semantics a schema cannot express (alp-sdk)

**Files:**
- Modify: `scripts/validate_metadata.py`
- Test: `tests/scripts/test_model_perf_metadata.py`

Mirror `_check_npu_ops_semantics` (`scripts/validate_metadata.py:1149`).

- [ ] The filename encodes the identity, and `measured_on` / `toolchain` must
      agree with it — the same `backend == path.stem` cross-check npu_ops uses.
- [ ] `measured_on.sku` must be a SKU that exists under `metadata/e1m_modules/`.
- [ ] `measured_on.accel_config` must be one the SKU actually resolves
      (`resolve_targets`' own list), so a point cannot claim a target the module
      does not have.
- [ ] `measured_on.hw_rev` must exist in that family's `hw-revisions.yaml`.
- [ ] A point whose `model.source` names an in-repo path must match that file's
      real sha256 — catches a fixture regenerated without re-benching.
- [ ] **Mutation-prove every one of the above.** Break each in a throwaway copy,
      confirm RED, restore, confirm green. A metadata gate that passes on a
      broken point is the failure mode this programme has hit five times.
- [ ] Commit.

## Task 3: the capture recipe (alp-sdk docs)

**Files:**
- Create: `docs/bench/model-perf-capture.md`

A perf point is only worth what its reproducibility is. Write the recipe BEFORE
any bench time is spent, so the first campaign produces points someone else can
re-derive.

- [ ] The exact toolchain invocation (including `--system-config` / `--memory-mode`
      — see `2026-08-16-vela-memory-profile.md`, and note the open `.ini` question
      there: a point captured under vela's built-in default profile MUST record
      that profile, since the arena figures describe it and not the module).
- [ ] The flash + run procedure per SoM, referencing the existing bench skills
      rather than restating them.
- [ ] How latency is measured, over how many runs, and what is reported
      (`mean` + `p95` + `runs`, never a single shot).
- [ ] Where the raw capture goes (alp-sdk-internal) and what the public point
      cites.
- [ ] **What to do when a figure cannot be measured** — omit the field, never
      estimate it.

## Task 4: tiered resolution in tan

**Files:**
- Create: `python/tan/model/perf.py`
- Modify: `python/tan/model/check.py`, `python/tan/model/analyze.py`
- Test: `python/tests/model/test_perf.py`, `python/tests/model/test_check.py`

**Interfaces:**
- Consumes: Task 1's perf points via `metadata_root`.
- Produces: a `BackendReport` with `basis: "bench"` when a point matches.

Resolution order, per spec §3.5: **precomputed → exact-if-toolchain → static.**

- [ ] `find_perf_point(sku, backend, accel_config, model_sha256, metadata_root)`
      returns a point ONLY on an exact match of all five. A near-miss is not a
      match — no "closest model", no "same model, different toolchain version".
- [ ] On a match: `basis: "bench"`, `confidence: "certain"`, the measured arena /
      `req_sram_kib` / latency, and the capture reference so the number is
      traceable.
- [ ] On no match: fall through silently to the existing behaviour. Absence must
      never degrade a report that would otherwise have been produced.
- [ ] **The `fits` guard must extend to this path.** `basis: "bench"` is now the
      second surface permitted to emit it; add the case to the live-template test
      and mutation-prove it.
- [ ] A perf point whose `toolchain.version` differs from the locally installed
      one is still valid for tier 2 (it is OUR measurement, not the customer's) —
      but `--exact` must still win over it when the customer has the toolchain
      **and** their compile disagrees, because their profile may differ. Decide
      that precedence deliberately and pin it with a test.
- [ ] Commit.

## Task 5: the first real perf points (bench — NOT part of this plan's code)

**This task needs silicon and a held labgrid reservation. It is serial, it never
runs inside a workflow, and it is gated on Tasks 1-3 landing** — the schema must
exist before the bench produces data, or the campaign yields numbers in a shape
nothing can read.

- [ ] Capture `person_detect_int8.tflite` on `E1M-AEN801` at `ethos-u85-256` and
      `ethos-u55-256` per the Task 3 recipe.
- [ ] Verify a captured point round-trips: `tan model check` reports
      `basis: "bench"` with the measured figures and no local toolchain.
- [ ] Record what could NOT be measured and why.

---

## Open questions for the maintainer

1. **The `.ini` question from the vela plan blocks the meaning of these numbers.**
   Until a module vela profile is resolved, every Ethos-U point would be captured
   under vela's built-in default (`Ethos_U85_SYS_DRAM_Mid` / `Dedicated_Sram_384KB`)
   — a DRAM-backed profile on a part with no DRAM. A tier-2 point captured that
   way is exactly measured and describes the wrong machine. **Recommendation:
   resolve the profile before spending bench time**, or the campaign has to be
   redone.
2. **The const-region question likewise.** `req_sram_kib` currently reports the
   arena alone (72 KiB measured for person_detect) while ~307 KiB is genuinely
   SRAM0-resident. A bench point should record what is REALLY resident, which
   needs the per-part answer.
3. **Which models seed the first campaign?** The spec's zoo v1 names five —
   person-detection, image-classification (MobileNet), keyword-spotting,
   anomaly-detection, VAD. Only person-detect exists in-repo today.

## Deliberately NOT in this plan

- The tier-1 **calibration** loop (fitting a per-NPU throughput model to measured
  points). It needs several points per NPU before a fit means anything; the data
  shape here is designed to support it later.
- The **model zoo** (sub-project 2) and `tan model add` — this plan supplies the
  `perf_ref` a zoo entry will cite, nothing more.
- **Power** (`∫ V·I dt` per rail). The spec's §4 design needs per-SoM
  power-topology metadata (monitor IC, I2C address, shunt Ω, which rail feeds the
  NPU) that is explicitly pending the maintainer's authoritative hardware data.
  The schema leaves room; this plan measures none.
- The **cross-sell suggestion** ("a smaller in-family module fits"), which needs
  points across several SKUs before it can be honest.
