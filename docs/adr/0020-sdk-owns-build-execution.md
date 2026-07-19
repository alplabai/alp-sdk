# 0020. The SDK plans; a standalone `tan` CLI is the whole command surface (three repos, one executor)

Status: Accepted — implemented (end-state B; Phase 4 complete)
Date: 2026-07-18
Deciders: alpCaner (alp-sdk), Hakan (alp-sdk-vscode) — co-signed
Supersedes: [0014](0014-build-plan-emit-cli-contract.md) — its
mechanism clause **and** its 84-87 consequence (`west alp-build` stays native).
Pairs with RFC #837 (`alp` → `tan`).

> **Implemented.** The SDK-side executor, `west alp-build`, and every SDK-side
> user command are retired; the whole command surface now lives in the
> standalone, public **`tan` CLI** ([`alplabai/tan-cli`](https://github.com/alplabai/tan-cli)).
> alp-sdk is plans-only: `alp_orchestrate --emit build-plan` / `--emit
> system-manifest` are the sole outputs `tan` consumes (`tan build --native`,
> `tan build` / `flash` / `image` / `size` / `renode` / `clean` for
> hardware/manifest-driven runs). Phase 2 (the extraction) was Hakan's repos
> and his to drive; alp-sdk owned Phases 1 and 4. See *Migration* below for
> the phase history and *The one thing that must hold* for the completeness
> gate this satisfied before Phase 4 deleted the SDK-side executor.

## Context

### The problem

Two code paths turn a `board.yaml` into per-core builds: `Orchestrator.fan_out()`
(SDK Python, drives `west alp-build`) and the Rust CLI's executor (in
alp-sdk-vscode) that consumes `--emit build-plan`. RFC #843. The drift is real,
not hypothetical: the plan's per-slice `env` carried only
`{"ALP_SDK_ROOT": ...}` (`buildplan.py:384`), so the Rust executor hand-ported
`ZEPHYR_BASE` / `EXTRA_ZEPHYR_MODULES` + the skip-vs-fail policy
(`orchestrator.py:300-304`). ADR 0014's "cannot drift by construction" holds for
artefact *bytes*, not *execution semantics*, and it leaked within ~6 weeks while
both executors existed — invisible until a bench failure.

### The decision, and why B over A

- **A** — the executor stays in the SDK (Python `fan_out`); a thin manager
  dispatches to it. Keeps the SDK standalone-buildable + an in-repo build oracle;
  costs release-train coupling.
- **B** (this ADR) — the executor and the whole user command surface live above
  the SDK, in a standalone `tan` CLI; the SDK is a plans-only backend.

**B is chosen** (held up under adversarial review): Hakan's CLI-side ADR already
makes the native CLI the single executing surface; the "not standalone-buildable"
objection is weak (building always needs both an SDK checkout and the CLI, which
is a normal setup); Rust owns process/cancel/streaming natively; and it fully
decouples release trains. Honest costs — no in-repo build oracle, and it reverses
0014:84-87 — are accepted deliberately (recorded under *Consequences*). A is kept
under *Alternatives* as the reversible fallback until Phase 4.

## Decision

**alp-sdk plans; a standalone `tan` CLI is the entire user command surface and
the sole executor; the IDE shells `tan`. Three repos.** No `alp`-named command
survives anywhere (RFC #837).

1. **alp-sdk = planner + libraries + Python backend, ZERO user commands.** It
   ships the C/HAL/metadata and the Python planner/validator/generator logic
   that `tan` invokes (`--emit`, `validate_board_yaml.py`, `alp_orchestrate`).
   **Retired:** `Orchestrator.fan_out()` / `_dispatch_slice()`, the `west alp-*`
   extensions (build/flash/image/size/renode/clean), and the `alp` console
   script — all in Phase 4, not before the contract is complete.

2. **`tan` — a NEW standalone repo (Rust), the whole command surface.** Extracted
   out of alp-sdk-vscode's `cli-rs/` and grown to own **`tan build / flash /
   image / size / renode / clean / sdk / doctor / validate / …`**. It is the sole
   executor (runs `west` / `bitbake` / `cmake` per slice), owns skip-vs-fail, env
   application, scheduling, cancellation, progress UX, SDK lifecycle, **and both
   ends of the manifest** — it writes `system-manifest.yaml` + `.alp-build-state.json`
   (seeded by the planner's `--emit system-manifest`) and its own flash/size/image
   read them back. Independently versioned + distributed (npm shim / prebuilt).
   **This is what a standalone SDK user installs — without the VS Code
   extension.**

3. **alp-sdk-vscode = the extension only.** Thin; shells `tan` by explicit binary
   path. Bundles or downloads the `tan` binary **with pinned-hash / signature
   verification** (it executes arbitrary build commands — the download is the
   softest link otherwise).

   **One-way dependency: extension → tan → alp-sdk.** Installing `tan` never
   drags in the extension.

4. **Two seams, both resolved inside `tan`.** IN: the plan JSON
   (`--emit build-plan`) — the executor's only input. OUT: the manifest + state
   files — consumed by `tan`'s own flash/size/image. Because `tan` owns the whole
   surface, there is **no cross-repo output contract** — the manifest is internal
   to `tan`, seeded by the SDK emit it also consumes.

5. **Version-skew guard (prevents #843 via skew).** The contract grows additively
   (`schemaVersion 1`), so an *older* `tan` release could ignore a new field and
   silently fall back to hand-porting — re-creating the drift. Mitigation: the
   plan may mark a key **required-for-execution**; `tan` **fails loudly on an
   unknown required key** rather than falling back. (Not "drift cannot fork" — it
   can, via skew; this is the guard against it.)

The planner is **not** rewritten to Rust (illusory no-Python prize; `west` /
`bitbake` are Python; the planner is the fast-moving vendor-heavy half).

## The plan contract must be complete before the SDK stops executing

The plan is the executor's only input; after Phase 4 there is no SDK-side
executor to diff against. All of this lands (additively under `schemaVersion 1`)
while `fan_out` still exists to prove it:

- ✅ **`envAppendPath`** — SDK-owned `EXTRA_ZEPHYR_MODULES` / `PYTHONPATH` append
  (a new key, not stuffed into `env`; "plan wins / CLI fills gaps"; **append only
  if not already present**, matching the reference appenders' de-dup). *Done —
  item 3.*
- **`executionPolicy`** — publish skip-vs-fail (unknown-os = fail, missing-tool =
  skip, `command: null` = skip-with-warning; `orchestrator.py:294-304`).
- **`system-manifest` ordering** — `--emit system-manifest` emits declaration
  order (`manifest.py:37`) while `fan_out` writes sorted; share the ordering, not
  the skip filter (the manifest keeps `os: off` cores).
- **`--core` + `--emit` fail-fast** — `emit_build_plan` has no core-scoping, so
  `--core X --emit build-plan` silently emits every core; reject the combination.
- **slice-independence invariant** — committed contract property *with* its
  precondition: order-free / concurrency-safe **given all `sharedArtefacts` +
  `configArtefacts` are materialised first**; a future firmware-packaging slice
  must preserve it or the invariant version-bumps.
- **required-for-execution key list** — the version-skew guard above.
- **plans are trusted input** — a materialised plan carries absolute host paths +
  `PYTHONPATH`; same trust domain as `board.yaml`; the executor confines writes
  under `buildRoot` and resolves tools by explicit path.
- Fix **#840**; widen `test_emit_build_plan_matches_materialiser` to command +
  env parity.

## Migration — the plan gains everything before anything is deleted

**All four phases are complete.** `tan` (`alplabai/tan-cli`) is the sole
executor and whole command surface; the SDK-side executor and every SDK-side
user command are retired.

1. **Phase 1 (alp-sdk):** complete the contract above; `fan_out` stays and ideally
   consumes the new fields (self-parity). Ship on an SDK tag; delete nothing.
2. **Phase 2 (`tan`, Hakan):** extract `cli-rs` → the new `tan` repo and **grow it
   to the whole command surface** (build + flash/image/size/renode/clean + the
   manifest I/O those need). The executor conforms to the contract (deletes the
   hand-ported env / skip policy); add **build-validation CI** (SoM matrix,
   plan→execute).
3. **Phase 3 (parity window):** both executors alive; the completeness gate runs
   (see below).
4. **Phase 4 (alp-sdk):** retire `fan_out` / `_dispatch_slice` / the `west alp-*`
   extensions / the `alp` console script; migrate the in-repo consumers (below);
   mark 0014:84-87 superseded; `tan` is the only surface. **No rollback, no
   in-repo oracle after this.**

### Phase-3 parity gate — defined and runnable (satisfied before Phase 4)

"Released `tan` output == `fan_out` output" is undefined and unrunnable as a
slogan (no bitbake-capable CI runners exist; `fan_out` was never a real build
oracle for A-core/yocto — `pr-alp-build.yml` runs the orchestrator with
`continue-on-error`). Define it as:

- **(in) command + env + skip/fail-decision equivalence** between `fan_out` and
  `tan`'s dry-run of the same plan, over the **full SoM matrix**, captured
  **toolchain-free** (this is exactly the drift that motivated the ADR, and it
  compares without building);
- **(out) manifest + state byte-parity** on what each writes;
- **plus Zephyr-slice artefact parity** (buildable on today's runners —
  `pr-renode-aen-smoke.yml` already builds via `west alp-build`);
- **yocto/A-core artefact parity is explicitly OUT of scope** (no runner infra;
  bitbake isn't byte-reproducible).

### In-repo consumers Phase 4 must migrate (grep-verified)

- **CI that executes:** `pr-alp-build.yml` (manifest-shape/determinism),
  `pr-renode-aen-smoke.yml` (builds the M55-HP ELF), `pr-renode-dual-os.yml`
  (consumes that ELF) — repoint at a released `tan` binary.
- **Bootstrap / doctor:** `bootstrap.sh` / `bootstrap.ps1` (hard-`die` if
  `west alp-build` missing), `doctor.py` (asserts registration).
- **Manifest/state consumers** (move with the surface to `tan`):
  `flash_backends`, `check_system_manifest.py`, the `flash/image/size/renode/clean`
  commands.
- **Docs (~25):** `README.md`, `AGENTS.md`, `llms.txt`, `getting-started.md`,
  `cli.md`, tutorials, example READMEs — rewrite `west alp-*`/`alp` → `tan`.

### Cross-repo oracle trigger

alp-sdk's planner is the fast-moving half; a planner change that emits fine but
builds wrong must not wait for `tan`'s next unrelated CI. alp-sdk CI fires a
`repository_dispatch` into `tan`'s build-validation workflow (or `tan` crons
against alp-sdk `dev` HEAD) and the result surfaces back as a status on the
alp-sdk PR.

## CI coverage (split)

- **alp-sdk CI:** emit shape / schema / determinism + emit↔`fan_out` parity
  (until Phase 4). No build execution.
- **`tan` CI:** plan→execute build validation over the SoM matrix — build
  correctness lives here; triggered cross-repo on alp-sdk planner changes.
- **alp-sdk-vscode CI:** the extension shells `tan`.

## Consequences

**Good**
- Exactly one executor and one command surface; the #843 drift class is removed
  for HEAD-vs-HEAD, and the skew re-introduction is closed by the
  required-key guard.
- `tan` is a first-class standalone product; installing it never pulls the
  extension (extension → tan → alp-sdk, one-way).
- Rust owns process/cancel/streaming; the SDK sheds all host/process/command
  code; release trains fully decoupled.

**Bad / costs**
- **No in-repo build oracle** — alp-sdk CI asserts plan shape only; build
  correctness is caught in `tan` CI (mitigated by the cross-repo trigger).
- **Reverses 0014:84-87** — pure-`west`-native build without `tan` ends.
- **The plan contract is a fragile single point** post-Phase-4 — mitigated only by
  completing it and proving it in Phase 3.
- **Large, unpriced-until-now scope on Hakan's side:** Phase 2 is not "extract the
  executor" but "extract the **whole command surface** + manifest I/O + stand up a
  new repo's CI/release + build-validation infra (incl. any self-hosted runners
  the SoM matrix needs)."

**Security:** plans are trusted input; the executor confines writes under
`buildRoot`, resolves tools/interpreters by explicit path (never PATH); the
extension verifies the `tan` binary by pinned hash / signature before running it.

## Alternatives

1. **A — executor stays in the SDK (Python).** Keeps standalone-build + the
   in-repo oracle + reversibility. Not chosen (contradicts Hakan's CLI-side ADR;
   "both repos present is fine" removes its edge). **Retained as the fallback** if
   a Phase-1..3 blocker appears before the irreversible Phase 4.
2. **Keep `west alp-build` shelling `tan`** — rejected (a west workspace would
   then depend on another repo's binary, the same 0014:84-87 clause).
3. **Shared Rust crate / Rust planner mirror** — rejected in 0014.

## The one thing that must hold

**Before Phase 4, the proven contract covers BOTH seams with a defined, runnable
comparator** — (in) per-slice command + env + skip/fail equivalence over the full
SoM matrix, runnable toolchain-free via dry-run capture; (out) manifest + state
byte-parity — **and an automatic cross-repo trigger runs `tan`'s build validation
on every alp-sdk planner change.** `fan_out` is the only thing that can validate
the contract from inside alp-sdk and it is the thing being removed, so
completeness must be proven while it still exists. Green-but-hollow parity
followed by Phase-4 deletion is the failure scenario (fan_out gone, flash/renode/CI
broken, no rollback).

## Open questions / asks for Hakan (resolved — see Status)

1. **`cli-rs` → standalone `tan` repo, grown to the whole command surface** —
   agreed in principle? This is materially larger than "extract the executor";
   it's your code + CI/release + build-validation infra to stand up. Timing is
   yours.
2. **Build-validation CI + the SoM-matrix runner infra** (incl. bitbake for
   A-core) — where does it run? This gates the Phase-3 completeness definition.
3. **Phase-3 parity mechanism** — the toolchain-free command/env/skip comparator +
   manifest byte-parity + the `repository_dispatch` trigger.
4. **`tan` naming** (#837) confirmed as the single surface; the Python `alp`
   console script is retired in Phase 4 so no `alp` remains.
