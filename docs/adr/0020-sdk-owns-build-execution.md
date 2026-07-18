# 0020. One executor, above the SDK: the SDK plans, a single Rust executor (beside the manager) builds

Status: Proposed
Date: 2026-07-18
Deciders: alpCaner (alp-sdk), Hakan (alp-sdk-vscode) — **awaiting Hakan's co-sign**
Supersedes (on acceptance): [0014](0014-build-plan-emit-cli-contract.md) —
its mechanism split **and** its 84–87 consequence ("`west alp-build` stays
native")

> **Proposal for joint decision — not accepted.** It reverses a decision
> co-owned with Hakan and gives up a standing principle (SDK
> standalone-buildability). It must not be implemented until Hakan co-signs,
> and it must not delete anything until the plan contract is proven complete
> (see *The one thing that must hold*). An adversarial review of this end-state
> (see *Consequences → the honest counter-case*) found it technically weaker
> than the alternatives on several axes; it is proposed because the deciders
> chose its tradeoff, not because it dominates.

## Context

### The problem: two executors, and a contract that already leaked

Two code paths turn a `board.yaml` into per-core builds: `Orchestrator.fan_out()`
(SDK Python, drives `west alp-build` / `alp build`) and the Rust CLI in
alp-sdk-vscode, which consumes `--emit build-plan` and re-implements the
executor. RFC #843 raised the drift risk. It is not hypothetical: the emitted
plan's per-slice `env` carries only `{"ALP_SDK_ROOT": ...}`
(`buildplan.py:384`), so the Rust executor **hand-ports** `ZEPHYR_BASE` /
`EXTRA_ZEPHYR_MODULES` and the skip-vs-fail policy (`orchestrator.py:300-304`).
ADR 0014 claimed the plan carries "everything the consumer needs" and that
plan and build "cannot drift by construction" — true for artefact *bytes*,
false for *execution semantics*, and it leaked within ~6 weeks of 0014 while
both executors existed and nobody noticed. That "invisible until a bench
failure" property is the core hazard.

### How we got to this shape (and the fact that reverses the morning's draft)

- **Manager vs engine (rustup/cargo).** A tool that lives entirely *inside*
  the SDK cannot bootstrap or upgrade the SDK it lives in (Hakan's objection).
  So a **manager** must sit *above* the SDK — `sdk install/switch/current`,
  `init`, dispatch — installed independently, standalone (the pre-SDK,
  therefore pre-Python, bootstrap is the genuine justification for a native
  binary). The manager **dispatches to** the build engine; it does not
  re-implement it. This is `rustup` (manager, outside) / `cargo` (engine,
  inside a toolchain).
- **The deeper fork: must the SDK build on its own?** The manager/engine split
  still leaves one question — does the *engine* live inside the SDK (Python,
  SDK stays standalone-buildable) or beside the manager (Rust, SDK becomes
  plan-only)? An earlier draft of *this* ADR chose the former ("A": one Python
  executor in the SDK; delete the Rust executor). **The deciders chose the
  latter ("B").** The new fact that overrides A: the maintainers accept giving
  up SDK standalone-buildability, in exchange for (a) full release-train
  decoupling — build behavior iterates on the CLI's cadence, not SDK tags — and
  (b) a single **Rust** executor, which handles process-groups / Job-Objects /
  live streaming / cancellation on Windows more robustly than Python's
  `subprocess` / `ProcessPoolExecutor` (the latter already carries a
  sequential-fallback for its Windows flakiness).

### Honest framing of the trade

B **diverges from the vendor precedent** the earlier draft leaned on: Zephyr
(`west`), NXP (west extensions), and ST (CubeCLT) all keep build orchestration
in the scripting/meta-tool layer and make the IDE a thin face over it. B moves
execution *out* of that layer into a native binary. What B buys for that price:
release-train decoupling and Rust's process handling. What it costs: the SDK
can no longer build multi-core projects from a Python checkout alone (reverses
0014:84–87 and the "standalone usage is first-class" principle), and alp-sdk
loses its in-repo end-to-end build oracle. These are real; they are recorded in
*Consequences* so the co-sign is informed.

## Decision

**The SDK plans; a single executor above the SDK builds.**

1. **alp-sdk (Python) = planner + libraries only.** It emits the build plan
   (`python -m alp_orchestrate --emit build-plan`, JSON, schema-versioned) and
   ships the C/HAL/metadata. It **no longer executes builds**:
   `Orchestrator.fan_out()` / `_dispatch_slice()` and the `west alp-build` west
   extension are **retired** (on the schedule in *Migration* — not before the
   contract is complete).

2. **One executor, in the CLI repo (alp-sdk-vscode), Rust.** It consumes the
   plan, materialises artefacts, runs `west` / `bitbake` / `cmake` per slice,
   and owns skip-vs-fail, env application, scheduling, incremental cache,
   cancellation, and progress UX. There is exactly one executor anywhere.

3. **Manager (Rust, above the SDK)** owns SDK lifecycle + dispatch; the IDE
   extension shells the manager. Manager and executor are the same repo /
   binary family. `alp sdk switch <ver>` upgrades the SDK from outside it.

4. **Language boundary — placed at the plan JSON:**

   | Layer | Language | Why |
   |---|---|---|
   | Manager + executor (spawn, cancel, Job-Object/process-group, streaming, IO) | Rust | systems mechanism; better process control; the part worth the native binary |
   | Planner + validators + generators + metadata tooling (routing, DTS/Kconfig/sysbuild derivation, vendor config) | Python | fast-moving, vendor-heavy (the planner doubled 1547→3066 lines in three v0.6 weeks); scripting keeps vendor-N+1 cheap |
   | Seam | plan JSON (`--emit build-plan`) | versioned, language-agnostic, already shipped |

   The planner is **not** rewritten to Rust: the "no-Python" prize is illusory
   (`west`/`bitbake` are Python; the build host has Python regardless), and
   mirroring the fast-moving planner is the re-implementation tax 0014 already
   rejected.

## The plan contract must become complete before the SDK stops executing

Once the SDK has no executor, the plan JSON is the **only** channel — the Rust
executor cannot fall back to reading SDK internals, and there is no SDK-side
executor left to diff against. Every item below must land in the plan (or be
explicitly assigned/scoped in this ADR) **while `fan_out` still exists to prove
it**:

1. **Per-slice env with append semantics.** Today `env` is a verbatim
   `{"ALP_SDK_ROOT": ...}`. Real dispatch **appends** the SDK path to
   `EXTRA_ZEPHYR_MODULES` and `<sdk>/scripts` to `PYTHONPATH`
   (`os.pathsep`-joined). A verbatim string map cannot express append — add a
   sibling key (e.g. `envAppendPath: {VAR: [values]}`). Changing `env`'s
   meaning bumps `schemaVersion`; a sibling is additive.
2. **`ZEPHYR_BASE` / west-workspace identity.** The SDK never sets
   `ZEPHYR_BASE`; it relies on `west`'s topdir discovery from cwd/shell. An
   IDE-spawned executor has neither — this is exactly the current Rust
   hand-port. The plan must carry the west topdir (or this ADR must assign its
   resolution to the executor explicitly), or the drift class survives day one.
3. **Skip-vs-fail / missing-tool policy** (`orchestrator.py:294-304`): unknown
   `os` → fail; tool absent from PATH → skip with the user-facing reason;
   `command: null` → skip-with-warning, never fail. The plan carries
   `command.tool` but zero policy — add an additive `executionPolicy`.
4. **`system-manifest.yaml` production — the biggest gap.** `fan_out` step 6
   writes `build/system-manifest.yaml`; `west alp-flash` hard-fails without it
   (`alp_flash.py:148-150`) and reads `flash_method` / `flash_args` from it.
   Retiring `fan_out` without a manifest story breaks the **flash/run chain**,
   including for the standalone `west` users B still claims to serve. The
   manifest mixes plan-time facts with run-time results (per-slice status,
   output artefact), so it cannot be a pre-rendered artefact — the ADR must
   decide: planner emits a status-free manifest template (semantic change for
   `alp-flash`), or the executor fills it (derivation leaking into the
   executor, which B forbids). **Must be resolved before flash is touched.**
5. **Cache-key inputs (#591).** `_slice_hash` walks `appDir` contents (skip-set
   `{.git, build, __pycache__}`) plus the SDK-version token — hashing less
   shipped a stale-artifact bug. If the Rust executor hashes only plan bytes,
   #591 ships again. The data (`appDir`) is in the plan; the *semantics* must
   be specified.
6. **Plan ephemerality.** The Zephyr command embeds the emitting interpreter
   (`-DPython3_EXECUTABLE={sys.executable}`, `orchestrator.py:546`) and absolute
   `SB_CONF_FILE` paths into the SDK checkout. A plan cached across `alp sdk
   switch` or a venv rebuild points CMake at a dead interpreter. **The plan is
   ephemeral: re-emit per build, never cache across an SDK switch.**
7. **Yocto execution environment.** The command is bare `bitbake <recipe>` with
   `cwd=buildDir` — no `oe-init-build-env`, no BBPATH/BUILDDIR/layers. The SDK
   executor is equally naive and only "works" because bitbake is absent on dev
   hosts (→ skipped). Under B the Rust executor must own the Yocto env with no
   reference — either specify it in-plan or declare it explicitly out-of-scope.
8. **Concurrency-safety.** Scheduling is the consumer's, but the plan must say
   which slices are parallel-safe (two `west build`s fine; anything sharing a
   Yocto tree not) — additive hint or documented rule.
9. **Ordering.** Shared artefacts materialise before any slice dispatch
   (implicit in `fan_out`, stated nowhere) — make it a contract rule.
10. **`--core` scoping fail-fast (#603).** Unknown core = error, not a silent
    all-skip manifest. This logic moves to the executor; unspecified, #603
    regresses.

**Schema impact.** Items 1, 3, 8–10 are additive under 0014's rule. Two bump
pressures: the manifest (item 4) likely forces a semantic change, and the
shipped `build-plan-v1.schema.json` uses `"additionalProperties": false`, so a
strict pinned consumer *rejects* additive keys — the executor must be required
to tolerate unknown keys, or the schema relaxes. Net: closing the contract most
likely lands a **build-plan `schemaVersion` 1→2** bump, coordinated with
consumers per 0014's CHANGELOG rule. (Note: the `schemaVersion:2` fixture in
commit 5053ce92 is the *board.yaml* schema — a different artifact; do not
conflate with the build-plan envelope, still `const: 1`.)

## Migration — the plan gains everything before anything is deleted

Each step is a shippable, tagged state; nothing is deleted in the tag that adds
its replacement; **`fan_out` is the oracle that proves the contract complete and
is deleted last.**

1. **SDK tag N — close the contract additively.** Land every item above.
   Crucially, make `fan_out` itself *consume the new plan fields* — the emitter
   and the last Python executor then read the same fields (self-parity: the
   strongest correctness proof available, and only while `fan_out` exists).
   Delete nothing.
2. **Rust executor rewritten against tag N** — deletes `zephyr_env_overrides`
   and the hand-ported skip policy; runs *only* what the plan says. Parity
   window: both executors alive.
3. **Cross-repo CI gate becomes required in alp-sdk** — downloads the
   **released** Rust executor and runs plan→execute over the SoM matrix,
   diffing outcome + manifest. (A released binary in CI is not a second
   implementation.)
4. **SDK tag N+1** — deprecate `west alp-build` (runtime warning), docs
   cutover, manager/IDE cutover.
5. **SDK tag N+2** — delete `fan_out` / `_dispatch_slice` / `west alp-build`;
   mark 0014:84–87 superseded. **After this there is no rollback and no in-repo
   oracle.**

## SDK self-test after `fan_out` is gone

alp-sdk CI can then only assert plan *shape* (existing `tests/scripts/
test_orchestrate_buildplan.py`) — it cannot prove a plan *builds*. The
mitigation is the required cross-repo gate (step 3), with one named limit: it
runs a *new* SDK's plan against an executor built for the *previous* tag, so it
only works if the executor contractually ignores unknown plan keys, and it can
never prove a plan feature needing new executor capability until the executor
releases (chicken-and-egg on every schema addition). A plan-level "dry-run"
assertion is worthless — it re-tests shape.

## Security

Under B a plan JSON is an **execution vector**: the executor byte-writes
`contents` to `path`s and runs `command.tool` with plan-supplied args. The
executor MUST confine writes under `buildRoot` (reject absolute / `..`-escaping
paths), resolve tools by explicit/validated path (never PATH — the two-`alp`
name collision makes PATH ambiguity a real hijack surface), and treat a
workspace-supplied plan as untrusted input.

## Consequences

**Good**
- Exactly one executor anywhere — user-visible build behavior cannot fork.
- Drift becomes "executor vs written contract" (visible in review) instead of
  "executor vs executor" (invisible until a bench failure).
- Full release-train decoupling: build/progress/cancel iterate on the CLI's
  cadence.
- Rust owns process/cancel/streaming — the Windows Job-Object / process-group
  story the IDE needs, done where it's strongest.
- The SDK sheds all host/process/OS lifecycle code — planner + libraries only.

**Bad / costs**
- **SDK is no longer standalone-buildable.** Multi-core orchestration — the
  SDK's headline feature — becomes impossible from a Python checkout. Reverses
  0014:84–87 and the standalone-first-class principle. Hand-written-firmware
  users must obtain the external executor to build.
- **No in-repo falsifier.** After step 5, skip policy, env derivation, cache
  inputs, and manifest production are planner-domain decisions enforced only by
  prose + schema in a repo that cannot execute them. The plan contract is a
  *more* fragile single point than two-executors-with-a-parity-test (which had
  a live diff oracle).
- **On-call inversion.** An executor bug now bricks SDK builds and only the CLI
  repo's release train can fix it — Hakan becomes on-call for the correctness
  of a planner he doesn't own.
- **Lockstep releases.** Every planner change with execution implications needs
  a coordinated executor release.

**The honest counter-case (what Hakan can fairly argue):** on the technical
merits an adversarial review preferred either A (executor in the SDK — keeps
the oracle, matches vendor precedent, keeps standalone build) or the staged
alternative (close the plan leak + parity gate, keep `west alp-build`), noting
that the staged alternative *is literally phase 1 of B's own migration* — cheap,
reversible, and a valid stopping point. B is chosen for its tradeoff
(decoupling + Rust executor), not because it wins on correctness. If the IDE
has a hard requirement a Python engine demonstrably cannot meet, name it here —
it is the strongest support for B.

## Alternatives

1. **A — executor stays in the SDK (this ADR's earlier draft).** One Python
   executor; the CLI is a thin manager that dispatches to it; SDK stays
   standalone-buildable; keeps the in-repo oracle and vendor-precedent
   alignment. Rejected by the deciders in favor of decoupling + a Rust
   executor, accepting the loss of standalone build.
2. **Staged (stop after phase 1).** Close the plan-completeness list + add the
   cross-repo parity gate, but keep `fan_out` / `west alp-build`. Two executors,
   now with a live parity oracle. Strictly less risky; the recommended
   *starting* point regardless (it is B phase 1) and a legitimate place to stop
   and re-evaluate.
3. **Shared Rust `alp-build-core` crate** — rejected in 0014 (forces a rewrite
   of working Python, couples release trains).
4. **Rust planner mirror** — rejected in 0014 and here (fast-moving vendor-heavy
   planner; standing re-implementation tax; illusory no-Python prize).

## The one thing that must hold

**The seam is proven complete by an executable gate before the second
implementation is deleted.** Every contract item lands in SDK tag N; a required
cross-repo CI job runs plan→execute green on the tagged SDK+executor pair; only
a later tag deletes `fan_out` / `west alp-build`. Completeness must be proven
while the oracle still exists — after deletion there is nothing left to prove it
against.

## Open questions for Hakan

1. **The tradeoff itself.** Standalone-buildability + the in-repo oracle +
   vendor-precedent alignment, given up for release-train decoupling + a single
   Rust executor — is that the right call, or do we stop at the staged phase 1?
2. **Is there a hard IDE requirement a Python engine can't meet** (Windows
   process ownership / Job-Object cancellation, progress granularity)? If yes,
   it is B's strongest justification and belongs in the Context.
3. **`system-manifest.yaml` (contract item 4)** — planner emits a status-free
   template, or executor fills it? This gates the entire flash/run chain.
4. **Yocto execution env (item 7)** — in-plan, or explicitly out-of-scope for
   the first executor?
5. **Schema bump** — accept build-plan `schemaVersion` 1→2 to close the
   contract, and relax `additionalProperties`?
6. **Naming** — settle with RFC #837 (`alp` → `tan`): which binary owns the
   surface name; the engine is invoked as `python -m alp_orchestrate`.
