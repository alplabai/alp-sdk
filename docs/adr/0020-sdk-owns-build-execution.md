# 0020. The SDK owns build execution — a thin manager above the SDK dispatches to one engine inside it (rustup/cargo split)

Status: Proposed
Date: 2026-07-18
Deciders: alpCaner (alp-sdk), Hakan (alp-sdk-vscode) — **awaiting Hakan's co-sign**
Supersedes: the **mechanism clause** of [0014](0014-build-plan-emit-cli-contract.md)
(the planner/`--emit build-plan` contract itself is retained — see Decision)

> **This ADR is a proposal for joint decision, not an accepted contract.**
> It reverses a decision co-owned with Hakan (0014) and must not be
> implemented until Hakan co-signs. See *Open questions for Hakan* at the end.

## Context

### What 0014 decided, and why we are re-opening it

ADR 0014 (Accepted 2026-06-04, amended 2026-07-18) split build responsibility
across the two repos: the **SDK owns the planner** (`scripts/alp_orchestrate`
+ `--emit build-plan`), and the **CLI owns the mechanism — materialise,
execute, schedule (parallelism + incremental cache), progress UX, JSON
envelope**. The plan deliberately omits `inputHash` and `sequential` because
"the consumer computes its own cache key" and "parallelism policy belongs to
the consumer's scheduler." The stated purpose was **decoupling the two repos'
release trains** — the Rust CLI in alp-sdk-vscode could iterate scheduling,
caching, and progress UX without waiting on an alp-sdk tag.

That was the right call given the framing *at the time* (a native Rust CLI at
the top of the build, a separate release train). Two facts have since changed
the framing:

1. **The maintainers now want consolidation.** The standing direction across
   the recent design thread is "`alp` CLI everywhere; `west` is a backend the
   `alp` CLI drives; the CLI may live in alp-sdk itself." That is the opposite
   of the two-repo-decoupling premise 0014 optimised for.
2. **0014's central safety property does not hold in practice** (below).

### 0014's "cannot drift by construction" is already false for env + skip-policy

0014 asserts the plan carries "everything the consumer needs to materialise +
run the build" and that "the plan and the on-disk build cannot drift by
construction." That is true for the *artefact bytes* (`_shared_artefacts` /
`_slice_config_artefact` are single-sourced) but **false for execution
semantics**:

- The emitted plan's per-slice `env` carries only `{"ALP_SDK_ROOT": ...}`
  (`scripts/alp_orchestrate/buildplan.py:384`). It does **not** carry
  `ZEPHYR_BASE` or `EXTRA_ZEPHYR_MODULES`.
- So the Rust executor **hand-ports** environment derivation the plan never
  described (`zephyr_env_overrides()`), plus the **skip-vs-fail** toolchain
  policy (a Rust re-implementation of `orchestrator.py:300-304`, citing
  alp-sdk#114), plus workspace-venv `west` resolution.

Each of those is a build **decision** re-implemented in a second language,
in a second repo, against a moving Python reference. This is the exact
"second executor" drift class RFC #843 raised — and there is no end-to-end
test that would catch a divergence.

### There are two binaries named `alp`, and one is already the thin CLI 0014 assigned to the other repo

- **Python `alp`** ships from this repo today: `pyproject.toml` →
  `[project.scripts] alp = "alp_cli.main:cli"`, a click CLI under
  `scripts/alp_cli/` (build / flash / run / doctor / emit / validate / …).
  Its `build.py` header states verbatim: *"Both paths are thin wrappers: no
  orchestrator logic lives here"* — it already delegates to
  `python -m alp_orchestrate`, and `scripts/alp_cli/_workspace.py` already
  does the `ALP_SDK_ROOT` / `EXTRA_ZEPHYR_MODULES` / `PYTHONPATH` env wiring
  the Rust executor hand-ports.
- **Rust `alp`** ships from alp-sdk-vscode. Its `doctor` actively probes for a
  "shadowing python `alp`" on PATH — the name collision is real enough that
  the Rust tool warns about it.

So the "thin CLI surface over one engine" is not something to *build* — it
**exists** in Python, in this repo. The confusion 0014's split produced is
literally two same-named binaries, one of which duplicates the other's engine.

### SDK-side execution already exists

`Orchestrator.fan_out()` (`orchestrator.py:342`) already plans **and**
executes in-process: `_dispatch_slice()` (`:286-329`) runs each slice's
backend (`west build` / `bitbake` / `cmake`), applies the skip-vs-fail policy
(`:300-304`), and writes the `build/system-manifest.yaml`. `west alp-build`
and Python `alp build` both front it. "SDK owns execution" is therefore
already half-true — this ADR makes it the *single* execution path and retires
the duplicate.

### What SDK-side execution does NOT have yet (the real work)

`fan_out()` was written for a one-shot CLI, not an interactive IDE:

- `subprocess.run` **blocks** with child stdout redirected to a **log file** —
  no live stream.
- Parallel dispatch uses `ProcessPoolExecutor` (already flaky on Windows; the
  code carries its own sequential fallback).
- Cache state is written only **after all slices finish** (`_save_state()`).
- There is **no cancellation design** — kill the parent mid-build on Windows
  and the grandchildren (`west`→`cmake`→`ninja`, or `bitbake`) orphan, and the
  state file is stale or half-written.

The IDE's two marquee UX needs — **live progress** and **cancel** — land
exactly on the part of the SDK executor that does not exist yet. This ADR
prices that work explicitly (see Decision → *The engine↔surface contract* and
Consequences → *costs*); it is the single largest work item and it is **not**
a flag.

### The manager role: a tool above the SDK must upgrade it (Hakan's objection)

A tool that lives *entirely inside* the SDK cannot bootstrap or upgrade the
SDK it lives in — the VS Code extension needs something that can run *before*
any SDK is installed and can *replace* the active one (`alp sdk install`,
`alp sdk switch`). That "something" cannot be inside the thing it swaps.

This is the manager/managed split, and the canonical model is **`rustup` /
`cargo`**:

- **`rustup`** lives *outside* every toolchain, is installed globally and
  standalone, manages + upgrades toolchains, and **dispatches** to the active
  one. The `cargo` on PATH is a rustup **proxy** that forwards to the active
  toolchain's real `cargo`.
- **`cargo`** ships *inside* each toolchain, versioned with it. rustup never
  re-implements cargo — it *forwards*.

So the objection is correct and it corrects an earlier framing of this ADR
that treated "the CLI" as one thing. There are **two roles**: a **manager**
(above the SDK, standalone, ships independently — `sdk install/switch`, `init`,
dispatch) and an **engine** (inside the SDK, versioned with it — plan +
execute). The key property is that the manager **dispatches to** the engine;
it does **not** re-implement it. That keeps *both* invariants: a tool above the
SDK can upgrade it (Hakan), *and* build execution has a single implementation
with no drift (RFC #843). Today's Rust `alp` already carries the manager role
(`sdk list/install/current/switch` exist in it); it merely *also* grew an
executor, which is the part this ADR deletes.

### Industry precedent

Every comparable embedded vendor keeps the build orchestrator in the
scripting/meta-tool layer and makes the IDE a thin subprocess face over the
*same* CLI that CI and terminal users call:

- **Zephyr** — `west` (Python meta-tool) + CMake/Ninja; vendors add **west
  extension commands** (Python plugins), never a systems-language re-write.
- **NXP MCUXpresso** — SDK "built around west, CMake, Ninja, Python"; NXP
  ships west extension commands; the VS Code extension "integrates with the
  underlying west build system" by shelling it.
- **ST STM32** — STM32CubeCLT is the CLI toolset "packaged for command-prompt
  use by third-party IDEs, or CI/CD"; the IDE shells those tools.

alp-sdk *is* the unification layer that sits where `west` sits for Zephyr, but
broader (it routes each core to `west` / `bitbake` / `cmake`). That routing is
the SDK's reason to exist; re-implementing it (or its execution) in a second
language is the one thing none of these vendors do. 0014's own rejected
"Rust planner mirror" alternative already reasoned this way ("the 'no Python'
prize is small"); this ADR extends the same reasoning from the *planner* to
the *executor*.

## Decision

**Split the CLI into two *roles* — a manager above the SDK and an engine
inside it — modelled on `rustup` / `cargo`. The manager owns lifecycle +
dispatch and re-implements nothing; the engine owns plan + execute + progress,
as one implementation.** (This replaces the earlier two-tier framing of this
ADR after Hakan's bootstrap objection — see Context → *The manager role*.)

1. **Manager `alp` — above the SDK, standalone, independently shipped**
   (the current Rust `alp` in alp-sdk-vscode *is* this role, kept). Owns:
   SDK **lifecycle** (`sdk install / switch / current`), `init`, pre-SDK
   `doctor`, the genuinely-offline verbs (`validate --offline`, `explain`,
   `diff`, `presets`), and — for every in-workspace verb (`build / flash /
   run`) — **dispatch**: resolve the *active* SDK's venv interpreter and
   exec-forward to the engine by **explicit path**, rendering its progress
   stream, mapping exit codes 0–5. It contains **no planner and no executor
   logic**. `alp sdk switch <ver>` is the manager upgrading the SDK from
   *outside* — the tool that replaces the SDK is never inside it (rustup).

2. **Engine — inside the SDK (Python), one implementation.**
   `alp_orchestrate` (fronted by the SDK-internal thin `alp_cli` verbs) owns
   the full build: board.yaml → per-core backend routing → materialise →
   execute → **structured progress stream** → manifest, plus skip-vs-fail,
   env derivation, venv/toolchain resolution, scheduling, and incremental
   cache. Single machine-readable source, matching the SDK's core principle.
   Reached only as `python -m alp_orchestrate <verb> --format jsonl` (via the
   manager's dispatch) — **never as a second binary named `alp` on PATH**.

3. **IDE extension** (alp-sdk-vscode, TypeScript) shells the **manager `alp`**
   via subprocess (its existing model). The manager owns interpreter
   resolution, so the extension needs no venv knowledge — it asks the manager
   to `build`, the manager forwards to the active SDK's engine.

4. **The Rust binary keeps the manager role, loses the executor role.**
   Deleted: `execute_slices`, `materialise_plan`, `zephyr_env_overrides`, the
   hand-ported skip-vs-fail. **Kept and now first-class:** SDK lifecycle
   commands, offline verbs, and venv/interpreter **resolution**
   (`find_workspace_venv`, `resolve_python_binary`) — a dispatcher must locate
   the Python it forwards to. Rust is justified *here*: the manager bootstraps
   **before** any SDK (hence before a guaranteed Python) is installed, so a
   standalone binary for the outer tool is warranted — this is the Rust
   rationale Hakan's objection supplies, which the "no-Python-prize-is-small"
   reasoning (true for the *engine*) never covered.

5. **`--emit build-plan` is retained** (0014's planner contract stands) for its
   remaining consumers — materialise-only tooling, CI, alp-studio — but its
   role as an *independently-executed* input is retired. The plan describes; it
   is no longer re-executed by a foreign re-implementation.

6. **Name collision resolved:** exactly one `alp` is on PATH — the **manager**.
   The engine is an SDK-internal module, not a competing PATH binary.

### The engine↔surface contract (replaces 0014's materialise/execute/schedule split)

0014's plan-as-executable-artefact contract is replaced, for the execution
path, by a **JSONL progress event stream** on the engine's stdout. Draft
shape (the exact schema is an item for Hakan review, versioned like the
build-plan schema):

| event | payload | purpose |
|---|---|---|
| `plan` | the resolved plan (echo of `--emit build-plan`) | consumer sees the full fan-out up front |
| `slice-start` | `{coreId, os, tool, command, cwd}` | one per non-`off` core |
| `slice-log` | `{coreId, stream: "stdout"\|"stderr", line}` | live child output (tee'd to the log file too) |
| `slice-end` | `{coreId, status: "built"\|"skipped"\|"failed"\|"cancelled", reason, durationMs}` | per-slice terminal state |
| `manifest` | `{path}` | points at `build/system-manifest.yaml` |
| `done` | `{exitCode}` | overall result; maps to exit codes 0–5 |

**Cancellation** is part of the contract: the engine dispatches each backend
in its own **process group** (POSIX) / **Job Object** (Windows) so a
SIGINT / CTRL_BREAK tears down the whole `west`→`cmake`→`ninja` (or `bitbake`)
tree; on cancel it emits `slice-end status:"cancelled"` and writes cache state
**atomically** (temp-file + rename) so a killed build never leaves a
half-written `.alp-build-state.json`. Dispatch moves from `ProcessPoolExecutor`
to **thread-based** workers (each worker just blocks on a subprocess), which
also removes the known Windows `ProcessPoolExecutor` flake.

This contract is **smaller and lower-stakes** than 0014's: a mis-rendered
progress line is cosmetic, whereas a mis-executed slice under 0014 is a broken
build. That asymmetry is the core argument for the collapse.

## Alternatives

1. **Stay inside 0014, close its proven leak (the "Option 1" we did not pick).**
   Put the *full* per-slice `env` + an explicit skip-policy field into the
   emitted plan (today only `ALP_SDK_ROOT`), so the Rust executor stops
   hand-porting and runs exactly what the plan says; add an emit↔execute
   parity test (RFC #843 done properly). Preserves release-train decoupling
   and requires no ADR reversal. **Rejected** because it only *narrows* drift:
   skip / schedule / cache *behaviour* stays duplicated, and `west alp-build`'s
   `fan_out` remains a second executor **forever** (0014 keeps it native). It
   is contract-patching where structural deletion is available — and it leaves
   the two-`alp` name collision unaddressed.

2. **Shared Rust `alp-build-core` crate both repos consume** — rejected in 0014
   (forces a rewrite of working Python, couples the release trains); still
   rejected, same reasons.

3. **Rust planner/executor mirror** — rejected in 0014 (the planner is
   fast-moving and vendor-heavy; a mirror is a standing re-implementation tax
   and a second source of truth); this ADR extends that reasoning to the
   executor.

4. **Three tiers (a new thin CLI layer distinct from `alp_cli`)** — rejected:
   it would reinvent `alp_cli`, which already exists and is already thin.

## Consequences

**Good:**

- Build execution has **one implementation**, in the repo where the engine
  already lives. The RFC #843 drift class is eliminated **structurally** — no
  parity test and no published execution-semantics contract are needed to
  *guard* a duplication that no longer exists.
- The two-`alp` name collision is resolved.
- The IDE, CI, and terminal users all drive the **same** engine — the vendor
  pattern (Zephyr / NXP / ST).
- `fan_out` gains live streaming, real cancellation, and atomic state — a
  strict UX upgrade for `west alp-build` and `alp build` too, not only the IDE.

**Bad / costs:**

- **Release-train re-coupling — but narrower than 0014 feared.** Only actual
  **build behavior** (the engine: schedule, cancel, progress *events*) now
  ships in an SDK release tag — which is correct, since a build must match the
  SDK that plans it. The **manager** (lifecycle, offline verbs, UX *rendering*,
  dispatch) still ships on the extension's own cadence, independently. So the
  extension's non-build interactivity is **not** hostage to SDK releases; only
  the executor is. This substantially softens the coupling 0014 optimised
  against — the manager/engine split is what buys that back.
- **No consumer-side hotfix path.** Once the CLI is a dumb spawner, an executor
  bug ships only via an SDK tag — there is no Rust-side patch. Accepted cost of
  "one implementation"; named here so it is chosen, not discovered.
- **Real net-new engineering** (not a flag): rewrite `_dispatch_slice` for
  tee-to-JSONL streaming, thread-based dispatch, process-group / Job-Object
  cancellation, and atomic cache-state writes; define + version the JSONL
  event schema; teach the extension to render it and to resolve the interpreter
  by explicit path.
- **Governance:** reverses a jointly-accepted ADR amended *today*. Requires
  Hakan's co-sign and a coordinated change in alp-sdk-vscode (gut the Rust
  executor). Until both land, 0014 remains in force.

**Security:**

- The extension (and any Rust shim) MUST resolve the venv interpreter by
  **explicit path** (`<venv>/Scripts/python.exe` / `<venv>/bin/python`), never
  a PATH lookup — the two-`alp` collision demonstrates PATH ambiguity is real
  in these workspaces, and a PATH-resolved `python`/`alp` in an untrusted
  workspace is an execution-hijack surface.

## Migration (once accepted)

Phased, each phase independently shippable and green:

1. **No-regret hardening (correct under 0014 *or* this ADR):** extract the
   single shared "sorted, non-`off` slice-set" enumeration both `fan_out` and
   `emit_build_plan` hand-code into one generator; make `Orchestrator`'s
   `board_yaml` **required** and delete the `Path.cwd()` fallback
   (`orchestrator.py:145-146` — a live "same board.yaml, two answers" bug,
   used only by tests today, `cli.py:80` already passes it).
2. **Engine streaming + cancellation:** the `_dispatch_slice` rewrite +
   `--format jsonl` on `alp_orchestrate` / `alp build`. Ship in an SDK tag.
3. **Extension cutover:** point the extension at the workspace-venv Python
   `alp` by explicit path; render the JSONL stream.
4. **Split the Rust roles:** delete the executor (`execute_slices`,
   `materialise_plan`, `zephyr_env_overrides`, hand-ported skip-vs-fail);
   keep + formalise the manager (lifecycle, dispatch-by-explicit-path,
   resolution, offline verbs). (alp-sdk-vscode PR, coordinated.)
5. **Docs + name:** `west alp-build` documented as the Zephyr-workspace-native
   alias; one binary keeps `alp`; on acceptance, mark 0014
   "Superseded by 0020 (mechanism clause)".

## Open questions for Hakan

1. **Release-train coupling — acceptable?** This is the crux you decided the
   other way in 0014. Is the extension's UX cadence riding SDK tags a price
   worth the structural no-drift + single implementation?
2. **Manager scope.** The Rust binary keeps the manager role (lifecycle +
   dispatch + offline verbs) — resolved. Remaining question: does the manager
   need any *other* native logic, and is a future napi/WASM embedding of
   `alp-core` in the extension still on the roadmap (it would let the extension
   call some manager logic in-process instead of by subprocess)?
3. **JSONL event schema** — does the draft above cover what the extension's
   progress/cancel UX needs (per-slice granularity, interleaved parallel
   output, structured error surfaces)?
4. **Sequencing** — does the alp-sdk streaming/cancellation work (phase 2)
   ship and stabilise on a tag *before* the alp-sdk-vscode cutover (phases
   3–4), so the extension is never mid-air?
5. **Naming** — settle alongside RFC #837 (`alp` → `tan`): which binary owns
   the surface name.
