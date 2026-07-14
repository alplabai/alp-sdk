# #610 §5 slice 1 — quality-task registry + drift gate

**Status:** design 2026-07-13
**Scope:** the *single source of truth for the SDK's quality gates* —
`metadata/quality-tasks-v1.json` + schema, a pure loader, a drift gate, and
**rewiring `scripts/test-all.sh` to read the registry** instead of its
hand-maintained `REQUIRED_GATE_SCRIPTS` bash array. This is the first focused
slice of §5 (the spec names #608 as the direct overlap). The profile RUNNER
(`alp quality --profile`), JSON/SARIF/JUnit orchestration, CI-job generation,
and change-impact planning are explicitly later slices.

## Goal

Kill the drift class #608 flagged: today `test-all.sh` hand-lists 17 "hard CI
gate" `check_*.py` in a bash array (`REQUIRED_GATE_SCRIPTS`) that silently
drifts from the 30 `scripts/check_*.py` CI actually runs. Make one machine-
readable registry the authority for *which* `check_*.py` gates exist, whether
each is a hard gate or informational, and which quality profiles
(quick/pr/full/release) include it — then have `test-all.sh` derive its list
from that registry, and a drift gate keep the registry honest against what's on
disk.

Developer flow:

```
python3 scripts/check_quality_registry.py   # registry == the check_*.py on disk; CI runs it
python3 scripts/quality_tasks.py --gate-scripts   # the hard-gate list test-all.sh consumes
```

## Non-goals (YAGNI)

- No `alp quality --profile` runner, no task execution engine (later slice).
- No JSON/SARIF/JUnit result emission (the exporters already exist for
  `alp validate` via diagnostic-v1; wiring them to a quality run is a later
  slice).
- No generation of CI workflow YAML from the registry, and no drift check that
  parses `.github/workflows/*.yml` (brittle; a later slice). Slice 1's drift
  gate is the registry ↔ `scripts/check_*.py`-on-disk ↔ `test-all.sh` triangle
  only.
- No change to any `check_*.py` gate's behavior; no new gate.
- Non-`check_*.py` CI jobs (twister, plain-cmake, doxygen, alp-build …) MAY be
  listed as tasks for completeness but are NOT drift-enforced in slice 1.

## Units

Each unit has one purpose, a defined interface, and is testable in isolation.
Mirrors the emit-registry-v1 (#664) shape: registry JSON + closed schema +
ast/glob drift gate.

### 1. `metadata/schemas/quality-tasks-v1.schema.json`

Draft 2020-12, `additionalProperties:false`. Root:
`{ schemaVersion:const 1, description:string, tasks:array<task> }`.
A `task`: `{ id (kebab, unique), description, runner (enum:
"check-script"|"shell"|"twister"|"cmake"|"build"), script? (path, required
when runner=="check-script"), command? (string, for runner=="shell"), gate
(bool — hard-fail in CI), profiles (array, non-empty, subset of
["quick","pr","full","release"]), output (enum: "none"|"junit"|"sarif"|"json"),
ci (string|null — the workflow job that runs it, informational in slice 1) }`.

### 2. `metadata/quality-tasks-v1.json`

One entry per `scripts/check_*.py` gate (all 30), each with its `gate` flag
(hard vs informational — `check_test_coverage.py` / `check_cross_platform.py`
are informational per test-all.sh:356) and `profiles`. The coarse test-all
stages (twister, plain-cmake, doxygen, generated-files) MAY be listed with
`runner != "check-script"` for completeness. `description` field states the
authority + stability contract (additive-only per schemaVersion), mirroring
emit-registry.

### 3. `scripts/quality_tasks.py` — pure loader + tiny CLI

Pure functions, stdlib `json` only (no PyYAML/ruamel — the registry is JSON):
- `load() -> dict`
- `gate_scripts() -> list[str]` — `script` of every task where `runner ==
  "check-script"` and `gate is True`, sorted.
- `check_scripts() -> list[str]` — every `check-script` task's `script`.
- `scripts_for_profile(profile) -> list[str]`.
- `main(argv)` — `--gate-scripts` / `--profile <p>` print one path per line
  (so `test-all.sh` consumes via `$(...)`).

### 4. `scripts/check_quality_registry.py` — drift gate

Mirrors `check_emit_registry.py`:
- validate registry against the schema (jsonschema Draft 2020-12);
- the registry's `check-script` set **exactly equals**
  `glob(scripts/check_*.py)` (minus the gate script itself) — no orphan gate
  (a `check_*.py` missing from the registry) and no phantom entry (a registry
  script with no file);
- every task has ≥1 profile and, if `runner=="check-script"`, a `script` that
  exists.
Fast, filesystem-only. Auto-listed in `catalog.json` via `gen_catalog.py`.

### 5. Rewire `scripts/test-all.sh`

Replace the hardcoded `REQUIRED_GATE_SCRIPTS=( … )` array (lines ~361-379)
with a registry-derived fill. **Portability (ADR 0012):** macOS ships bash 3.2
— no `mapfile`/`readarray`, so use a `while read` loop:

```bash
REQUIRED_GATE_SCRIPTS=()
while IFS= read -r _s; do REQUIRED_GATE_SCRIPTS+=("$_s"); done \
  < <(python3 "$REPO_ROOT/scripts/quality_tasks.py" --gate-scripts)
```

The stage that runs them is unchanged. Keep the array name so the rest of the
script is untouched. This removes the hand-maintained list entirely — the
registry (drift-gated in CI) becomes the single source.

### 6. Tests — `tests/scripts/test_quality_registry.py` (hermetic)

- schema is Draft-2020-12-valid + closed; `schemaVersion const 1`.
- committed registry validates against the schema.
- loader `gate_scripts()` returns only `gate && check-script` entries; `--gate-scripts`
  CLI prints them one per line.
- drift gate: a fixture registry missing an on-disk `check_*.py` fails; a
  phantom entry fails; the committed registry passes on the real tree.
- `scripts_for_profile("pr")` ⊆ `check_scripts()`.

## Data format — `quality-tasks-v1` (camelCase root, matches emit-registry)

```json
{
  "schemaVersion": 1,
  "description": "Single source of truth for the SDK's quality tasks ...",
  "tasks": [
    { "id": "doc-drift", "description": "...", "runner": "check-script",
      "script": "scripts/check_doc_drift.py", "gate": true,
      "profiles": ["pr", "full", "release"], "output": "none",
      "ci": "pr-doc-drift.yml:doc-drift" }
  ]
}
```

## CI wiring

- `check_quality_registry.py` added to the gate set (auto-listed in
  `catalog.json`; wired into `pr-metadata-validate.yml`'s validate job like the
  other registry gates).
- Because `test-all.sh` now derives its gate list from the registry, the local
  `pr`/`full` runs and CI reference the SAME source — the #608 drift is
  structurally impossible.

## Acceptance

- [ ] Schema + registry enumerate all 30 `scripts/check_*.py` with gate/profile
      flags; registry schema-valid.
- [ ] `quality_tasks.py` loader + `--gate-scripts`/`--profile` CLI.
- [ ] `check_quality_registry.py` drift gate (orphan + phantom + schema) + test.
- [ ] `test-all.sh` reads `REQUIRED_GATE_SCRIPTS` from the registry (hand array
      deleted).
- [ ] `catalog.json` regenerated; hermetic pytest green.

## Follow-ups (tracked, not built here)

- `alp quality --profile {quick,pr,full,release}` runner reading the registry.
- JSON/SARIF/JUnit result emission (reuse diagnostic-v1 exporters).
- Registry ↔ `.github/workflows/*.yml` drift check (prove each task's `ci`
  job exists and each CI gate is in the registry).
- Conservative change-impact planning from resolved manifests.
