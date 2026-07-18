# 0020. The SDK owns build execution; a thin manager above it dispatches and renders

Status: Proposed
Date: 2026-07-18
Deciders: alpCaner (alp-sdk), Hakan (alp-sdk-vscode) — **awaiting Hakan's co-sign**
Supersedes (on acceptance): the **execute/schedule/materialise** half of
[0014](0014-build-plan-emit-cli-contract.md)'s mechanism clause (moved back
into the SDK engine). **Retains** 0014:84–87 (`west alp-build` stays native)
and 0014's `--emit build-plan` planner contract; the CLI keeps progress
*rendering* + envelope.

> **Proposal for joint decision — not accepted.** It moves the executor role
> 0014 assigned to the CLI back into the SDK. It is deliberately **reversible**:
> Phase 1 forecloses nothing and is correct regardless of the endgame. Do not
> delete or cut over anything until Phase 1's parity gate is green.

## Context

### The problem: a contract that already leaked (live drift)

`--emit build-plan`'s per-slice `env` carries only `{"ALP_SDK_ROOT": ...}`
(`buildplan.py:384`), so the Rust CLI **hand-ports** `ZEPHYR_BASE` /
`EXTRA_ZEPHYR_MODULES` + the skip-vs-fail policy (`orchestrator.py:300-304`).
ADR 0014's "cannot drift by construction" holds for artefact *bytes*, not
*execution semantics* — and it leaked within ~6 weeks, invisibly, while two
executors (`Orchestrator.fan_out()` in the SDK; the Rust re-implementation in
alp-sdk-vscode) both existed. That "invisible until a bench failure" property is
the hazard RFC #843 raised, and it is real today.

### Manager vs engine (rustup / cargo)

A tool that lives entirely *inside* the SDK cannot bootstrap or upgrade the SDK
it lives in (Hakan's objection). So a **manager** sits *above* the SDK —
`sdk install/switch/current`, `init`, dispatch — installed independently,
standalone (the pre-SDK, therefore pre-Python, bootstrap justifies a native
binary). The manager **dispatches to** the build engine; it does not
re-implement it. `rustup` (manager, outside) / `cargo` (engine, inside a
toolchain).

### The fork we resolved: where does the ONE executor live?

The manager/engine split still leaves one question — is the *engine* inside the
SDK (Python) or beside the manager (Rust)? We examined both:

- **B (executor in the CLI repo, Rust):** better Windows process/cancel
  handling + full release-train decoupling — but **irreversible** (delete
  `fan_out`/`west alp-build` and the SDK can no longer build itself; no
  in-repo oracle), it reverses standalone-buildability and 0014:84–87, and it
  makes the plan JSON a fragile single point (the executor can no longer read
  SDK internals). An adversarial review judged it technically weaker.
- **A (this ADR):** the executor stays **in the SDK**. Keeps
  standalone-buildability, keeps the in-repo build oracle, matches the vendor
  precedent (Zephyr/NXP/ST all keep build orchestration in the scripting layer
  and make the IDE a thin face over it), and is **reversible**. Its cost —
  build behavior ships on SDK tags — is arguably *correct*: a build should
  match the SDK that planned it, and the manager still ships the IDE's non-build
  UX on its own cadence.

**A is chosen.** B remains on record (Alternatives) and would only win if the
IDE has a hard requirement Python's process model demonstrably cannot meet (see
Open questions).

## Decision

**One executor, in the SDK (Python). A thin Rust manager above the SDK
dispatches to it and renders.**

1. **SDK (Python) owns plan + execute.** `Orchestrator.fan_out()` remains the
   single executor and grows a **structured JSONL progress stream**
   (`--format jsonl`), **cancellation** (process-group on POSIX /
   `CREATE_NEW_PROCESS_GROUP` + Job Object on Windows so `west`→`cmake`→`ninja`
   / `bitbake` trees tear down cleanly), and **atomic** cache-state writes. The
   SDK stays **standalone-buildable** (`python -m alp_orchestrate build` works
   from a Python checkout); **`west alp-build` stays native** (0014:84–87
   preserved). Skip-vs-fail, env derivation, venv/toolchain resolution,
   scheduling, and cache all live here — one machine-readable source.

2. **Manager `alp` (Rust, above the SDK) is thin.** It owns SDK **lifecycle**
   (`sdk install/switch/current`), `init`, pre-SDK `doctor`, the genuinely
   offline verbs (`validate --offline`, `explain`, `diff`, `presets`), the JSON
   **envelope** + exit codes 0–5, and — for `build/flash/run` — **dispatch**:
   resolve the *active* SDK's venv interpreter (by **explicit path**, never
   PATH) and forward to `python -m alp_orchestrate <verb> --format jsonl`,
   **rendering** the engine's event stream. It contains **no planner and no
   executor**. Deleted from the Rust side: `execute_slices`, `materialise_plan`,
   `zephyr_env_overrides`, the hand-ported skip-vs-fail. Kept: SDK lifecycle,
   offline verbs, and venv/interpreter **resolution** (`find_workspace_venv`,
   `resolve_python_binary`) — a dispatcher must locate the Python it forwards
   to.

3. **IDE extension** shells the manager (its existing subprocess model). The
   manager owns interpreter resolution, so the extension needs no venv
   knowledge.

4. **`--emit build-plan` is retained** (0014's planner contract stands) for its
   other consumers — materialise-only tooling, CI, alp-studio. It is no longer
   the *execution* channel (the SDK executes directly); it stays a first-class
   *describe* surface.

5. **Language boundary:** Rust = manager (lifecycle + dispatch + render);
   Python = planner + **executor** + tooling. The seam is "manager invokes the
   engine and renders its JSONL stream" — **not** "manager re-executes a plan."
   The planner/executor is **not** rewritten to Rust (illusory no-Python prize;
   `west`/`bitbake` are Python; the planner is the fast-moving vendor-heavy half
   that doubled 1547→3066 lines in three v0.6 weeks).

### The `emit ↔ fan_out` single-source property

Under A, drift risk drops sharply — the executor (`fan_out`) reads SDK
internals directly, so it never depends solely on the plan JSON. The residual
concern is that `emit_build_plan()` and `fan_out()` are **two loops** over
`project.cores` that must agree (so `--emit` consumers see what a real build
does). Phase 1 makes them single-source and tests it.

## Migration — Phase 1 first, reversible, forecloses nothing

**Phase 1 — no-regret (do now; correct under A, B, or staged):**

1. **Extract the shared slice-set enumeration** (the "sorted, non-`off` cores"
   logic hand-coded in both `fan_out` and `emit_build_plan`) into one generator
   both call — so the two loops cannot diverge on ordering/filtering.
2. **Add the `emit ↔ fan_out` parity test** over a multi-core fixture (zephyr +
   yocto + baremetal + one `os: off` + one no-command core): assert the emitted
   plan's per-slice `command`/`env`/`buildDir` and slice set equal what the
   `Orchestrator` itself assigns. Toolchain-free.
3. **Close the emit contract leak additively** (for `--emit` consumers): carry
   the full per-slice env (env-append semantics via a new sibling key, not a
   verbatim string), an explicit skip-vs-fail `executionPolicy`, and the
   west-topdir/`ZEPHYR_BASE` fact — so `--emit` consumers stop hand-porting.
4. **Fix the live cwd bug:** make `Orchestrator`'s `board_yaml` **required**,
   delete the `Path.cwd()` fallback (`orchestrator.py:145-146`) — a
   "same board.yaml, two answers" divergence used only by tests today
   (`cli.py:80` already passes it).

Phase 1 fixes the real drift, keeps everything working, and commits to no
endgame. Ship in an SDK tag.

**Phase 2 — the A endgame (after Phase 1 is green):**

5. **Engine streaming + cancellation:** the `_dispatch_slice` rewrite (thread-
   based dispatch replacing the Windows-flaky `ProcessPoolExecutor`; tee child
   output to log **and** JSONL events; process-group/Job-Object cancellation;
   atomic state write) + `--format jsonl` on `alp_orchestrate` / `alp build`.
6. **Manager cutover (alp-sdk-vscode PR, coordinated):** delete the Rust
   executor; forward `build/flash/run` to the engine by explicit path; render
   the JSONL stream. Extension picks it up.

At no point is the SDK left unable to build (`fan_out` is only ever *extended*,
never removed).

## Consequences

**Good**
- Exactly one executor — build behavior cannot fork; the #843 drift class is
  removed at its source (the executor reads SDK truth directly).
- **SDK stays standalone-buildable**; the **in-repo end-to-end build oracle**
  survives; **`west alp-build` stays native** (0014:84–87 intact).
- Matches the vendor precedent (orchestration in the scripting layer; IDE is a
  thin face).
- **Reversible** — Phase 1 forecloses nothing; the endgame is a separate, later
  step.
- Smaller change than B (extend `fan_out` + delete the Rust executor; no whole
  plan-contract rewrite, no retirement of SDK build paths).

**Bad / costs**
- **Build behavior ships on SDK tags**, not the CLI's cadence. Mitigated: the
  manager (lifecycle, offline verbs, rendering) still ships independently — only
  execution rides SDK tags, which is arguably correct.
- **Python does process/cancel more manually than Rust.** `fan_out`'s current
  `ProcessPoolExecutor` is Windows-flaky and must be reworked to thread + Job
  Object for the Phase-2 streaming/cancellation. Feasible, but real work and the
  one place Rust would have been easier.

## Alternatives

1. **B — executor in the CLI repo (Rust).** Better process handling + full
   decoupling, but irreversible, loses standalone-build + the in-repo oracle +
   vendor-precedent alignment, and makes the plan JSON a fragile single point
   (10-item completeness gate before any deletion). Rejected as the endgame;
   would only win given a hard IDE requirement Python can't meet.
2. **Stop after Phase 1 (staged, two executors + parity).** Keep both `fan_out`
   and the Rust executor, made single-source by the parity test. Strictly least
   risky; a legitimate place to stop — it *is* A's Phase 1. A's Phase 2 is the
   optional consolidation on top.
3. **Shared Rust crate / Rust planner mirror** — rejected in 0014 (rewrite of
   working Python, coupled release trains, standing re-implementation tax);
   still rejected.

## Security

The manager resolves the venv interpreter by **explicit path**
(`<venv>/Scripts/python.exe` / `<venv>/bin/python`), never a PATH lookup — the
two-`alp` name collision makes PATH ambiguity a real execution-hijack surface.

## The one thing that must hold

**Phase 1's `emit ↔ fan_out` parity gate is green before any Phase-2 cutover.**
`fan_out` is only ever extended, never removed, so the SDK never loses its
ability to build itself or to serve as the in-repo oracle.

## Open questions for Hakan

1. **Is there a hard IDE requirement Python's process model can't meet**
   (Windows Job-Object cancellation, process ownership, progress granularity)?
   If yes, it is the one fact that argues for B over A — name it and we
   re-open the fork. If no, A stands.
2. **Release-train coupling** — build behavior on SDK tags (manager UX stays on
   the extension's cadence): acceptable?
3. **`--emit` contract closure** (Phase 1 item 3) — accept a build-plan
   `schemaVersion` 1→2 bump if the env-semantics change forces it? (The
   `schemaVersion:2` fixture in 5053ce92 is *board.yaml*'s — a different
   artifact.)
4. **Naming** — settle with RFC #837 (`alp` → `tan`): which binary owns the
   surface name; the engine is invoked as `python -m alp_orchestrate`.
