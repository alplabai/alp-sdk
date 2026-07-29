# Python `tan` — core executor MVP (sub-project 1 of 4)

Date: 2026-07-29
Status: Draft — for review
Deciders: alpCaner (alp-sdk), Hakan (tan-cli, alp-sdk-vscode)
Relates to: [ADR-0020](../../adr/0020-sdk-owns-build-execution.md) (this work
supersedes its language split; a superseding ADR-0022 is written once this MVP
retires the open risks)

## Why

Today the toolchain spans two languages either side of a versioned JSON seam:

```
alp-sdk (Python planner) ──build-plan JSON──► tan (Rust executor) ──envelope──► alp-sdk-vscode
```

That seam is not free. It costs a hand-synced parity comparator
(`tests/parity/seam1_field_diff.py` **and** its vendored twin inside tan-cli,
kept in lockstep by hand), a version-skew required-key guard, a cross-repo
`repository_dispatch` oracle that is still not standing (#855), and an
additive-only `schemaVersion 1` evolution discipline on every planner change.
ADR-0020 records this itself under *Bad / costs*: the plan contract is
"a fragile single point".

**The end-state:** planner *and* executor live in one Python program, `tan`.
alp-sdk keeps C/HAL, metadata, examples and firmware — and no Python. The
build-plan stops being a versioned inter-process contract and becomes an
in-process data structure, so the comparator, the skew guard and the #855
oracle are deleted rather than maintained.

The vscode↔`tan` envelope survives — it is a *process* boundary, not a language
boundary — but it is one contract instead of two.

### Why this is now viable

`tan` is the **sole hard consumer** of the build-plan. Studio's build-worker
runs `west build` directly and treats `alp build --plan` as purely additive
telemetry ("if the planner fails, the build proceeds and `planJson` stays
null"), and it already shells the Python `alp validate` surface. Nothing else
hard-depends on the plan JSON, so collapsing the seam strands no consumer.

## Scope of this sub-project

This spec covers **sub-project 1 only** — the risk-retiring MVP. The full port
is decomposed as:

| # | Sub-project | Delivers |
|---|---|---|
| **1** | **Core executor MVP** (this spec) | `build` / `build --native` in Python, hash-pinnable packaging, the parity oracle — on AEN + native_sim |
| 2 | Planner move | `alp_orchestrate` + validators/generators relocate into `tan`; the JSON seam collapses; comparator + skew guard + #855 machinery deleted |
| 3 | Surface breadth | flash / image / size / renode / clean + manifest I/O + the ~13 non-SDK commands |
| 4 | Cutover | vscode bundling, Studio, alp-sdk CI/docs/bootstrap repoint; Rust `tan` retired |

Sub-project 1 exists to answer three questions cheaply, **before** the planner
move touches alp-sdk or the cutover touches Hakan's extension:

1. Can a Python `tan` ship as a **hash-pinnable, signable single artifact**?
2. Does it reproduce the vscode **envelope** byte-for-byte?
3. Can we port **against a live oracle**, so no drift window opens?

If any answer is no, we learn it here and the remaining three sub-projects are
never started.

## Non-goals (explicitly out of scope for sub-project 1)

- Moving the planner. alp-sdk keeps its Python planner untouched; Python `tan`
  invokes `--emit build-plan` exactly as Rust `tan` does today. The seam
  collapse is sub-project 2.
- Any command beyond `build` and `build --native`.
- Any change to alp-sdk-vscode, Studio, or alp-sdk CI.
- Retiring Rust `tan`. It stays shipped and authoritative throughout — it is
  the oracle.
- V2N / NX9 / Yocto slices. AEN + native_sim only.

## Architecture

A new Python package inside the tan-cli repo, alongside the Rust workspace,
mirroring the Rust module split so parity review is a file-to-file read:

```
tan-cli/
  crates/            # unchanged, still shipped, still the oracle
  python/
    tan/
      __main__.py       # Typer app, `tan` entrypoint
      cli.py            # command surface (mirrors crates/tan-cli/src/cli.rs)
      envelope.py       # {command,ok,exitCode,project,data,issues} + exit codes
      core/             # pure domain — mirrors crates/tan-core
        build_plan.py   # plan consumer model + version-skew guard
        plan_exec.py    # apply_env_append / assemble_slice_env / resolve_action
      commands/
        build/          # native.py preflight.py execute.py materialise.py …
    tests/
      parity/           # the oracle harness (see below)
```

**`core/` is pure and IO-free**, matching the repo's existing
`keep-files-small; pure logic in tan-core, not the IO/executor file` rule. The
env-append de-dup and the skip-vs-fail policy are pure functions with unit
tests, exactly as `tan-core::plan_exec` has them.

### Framework choice

- **Surface: Typer.** Maps cleanly onto the existing `Commands` enum +
  per-command `XxxArgs` shape, and gives `--help` / completion for free.
- **Executor internals: stdlib `subprocess`.** No new dependency for process
  control. West itself is Python driving hour-long cmake/ninja/bitbake runs, so
  spawn / stream / cancel is a solved problem in this ecosystem, not research.
- **Progress:** a single small library (`rich`), replacing `indicatif`.

ADR-0020 claims "Rust owns process/cancel/streaming natively" as a benefit. We
treat that as a **capability** question that west already answers, and address
the *identity* half — one hashable artifact — under Packaging below.

## The three MVP deliverables

### 1. Executor: `build` and `build --native`

The full pipeline for a plan, contract-driven — no hand-ported behaviour:

- **Consume** the plan (`schemaVersion 1`). Top-level keys `schemaVersion`,
  `generatedBy`, `boardYaml`, `sku`, `buildRoot`, `slices`, `sharedArtefacts`,
  `warnings` are required; `executionPolicy`, `planPathMode`, `sdkVersion`,
  `sdkCommit` are optional-but-always-emitted (strict producer / tolerant
  consumer).
- **Version-skew guard**: reject an unsupported `schemaVersion`, and fail loudly
  on an unknown required key rather than falling back to hand-ported behaviour.
  That fallback is precisely the RFC #843 drift.
- **Token substitution**: `planPathMode: "tokened"` is required; substitute
  `${SDK_ROOT}` / `${PROJECT_ROOT}` / `${PYTHON}`.
- **Materialise** `sharedArtefacts` then each slice's `configArtefacts` **before**
  any slice runs — that ordering is the precondition of the plan's
  slice-independence invariant.
- **Env**: `env` set verbatim; `envAppendPath` appended `os.pathsep`-wise
  **only if not already present** (de-dup). "Plan wins / CLI fills gaps."
- **Policy**: honour `executionPolicy` — `unknownBackend`=fail,
  `missingTool`=skip, `nullCommand`=skip — falling back to built-in behaviour
  when an older plan omits it.
- **Consumer-side resolution stays consumer-side**: `ZEPHYR_BASE` and the west
  venv are deliberately *not* in the plan. Port the existing resolution (walk
  `.venv` up the tree, `$ZEPHYR_BASE/../.venv`, `<sdk-parent>/.venv`,
  `<sdk-parent>/zephyrproject/.venv`).
- **Execute** each slice, streaming output, honouring cancellation.

Per-slice fields consumed: `coreId`, `backend`, `buildDir`, `appDir`,
`configArtefacts`, `toolchain`, `artifacts`, `debug`, `command`, `env`,
`envAppendPath`.

### 2. Packaging — the hash-pin must survive

This is the load-bearing unknown. The extension today downloads a **pinned-hash,
signed single binary**; ADR-0020's security clause names that download "the
softest link… it executes arbitrary build commands". A Python program that
shells a user's site-packages cannot be hash-pinned, and `SUPPORTED_CLI_VERSION`
would degrade from a hash pin to an advisory string. That is an unacceptable
regression, so the MVP must prove it away.

**Approach: PyInstaller one-file build per platform**, producing a single
executable that is hashed and signed exactly like the Rust artifact — the
existing 8-asset release shape and the extension's verification flow are
preserved.

This does **not** claim `tan` becomes Python-free at runtime for *building*: a
build always needs the host's west/Zephyr Python environment. That is equally
true of Rust `tan` today (ADR-0020 refuses a Rust planner precisely because
west/bitbake are Python). The bundled interpreter runs `tan` itself; the west
venv runs the build. Keeping those two interpreters distinct and explicit is a
requirement, not an accident — resolve every tool by explicit path, never PATH.

**Acceptance:** a signed single-file `tan` on Linux/macOS/Windows whose SHA-256
the extension can pin; startup overhead measured and recorded.

### 3. The parity oracle — port against a live reference

Rust `tan` is shipped, working, and stays installed. For every ported
capability we diff **Python `tan` vs Rust `tan`** on the same plan:

- **envelope parity** — `{command, ok, exitCode, project, data, issues}` compared
  as parsed JSON (key order irrelevant, values exact), plus the process exit
  code;
- **execution parity** — per-slice resolved `command`, resolved `env`, and the
  skip-vs-fail decision, captured **toolchain-free via dry-run**. This is the
  same shape as the seam-1 comparator (`command`, `env`, `appDir`,
  skip/fail-decision, `debug.probe`), reused rather than reinvented;
- **artefact parity** — for the real AEN build, the materialised artefact set
  and the produced `zephyr.elf` / `zephyr.bin` exist and the build succeeds.

Rust `tan` is retired only in sub-project 4, and only for capabilities the
oracle has confirmed. **No oracle-less window opens at any point** — this is the
direct mitigation of the "reverse drift, no reference" risk, and it is why the
port is safe in a way the original Phase 4 deletion was not.

## Test strategy

- **Unit** (pytest) on `core/`: `apply_env_append` de-dup, `assemble_slice_env`
  precedence, `resolve_action` across the policy matrix, token substitution,
  and the version-skew guard's reject paths. Pure functions, no IO.
- **Parity** (the oracle above): dry-run capture over the AEN + native_sim
  matrix, asserted against Rust `tan`.
- **End-to-end**: `tan build --native --sdk-root <alp-sdk> --board-yaml
  examples/multicore/rpmsg-v2n/board.yaml` must compile a real `zephyr.elf` +
  `zephyr.bin`, matching the verified Rust e2e.
- **Packaging**: the PyInstaller artifact runs the e2e on a machine without a
  development Python install of `tan`.

## Error handling

Failures stay on the **value path**, not exceptions escaping to the top level —
every failure becomes an envelope with `ok: false`, a non-zero `exitCode`, and
structured `issues`. The existing exit-code mapping is preserved verbatim; the
extension parses these, so any divergence is an envelope-parity failure caught
by the oracle. Plan-level rejections (bad `schemaVersion`, unknown required
key, non-tokened `planPathMode`) fail loudly *before* any slice runs.

Plans remain **trusted input**, as today: writes are confined under
`buildRoot`, and tools/interpreters are resolved by explicit path.

## Risks

| Risk | Mitigation |
|---|---|
| Python artifact can't be hash-pinned → vscode security regression | PyInstaller single-file, signed + SHA-256 pinned. **Proven in this sub-project or the port stops.** |
| Reverse drift while porting, no reference | Rust `tan` is the live oracle for every capability; it is retired only after parity. |
| Process/cancel/streaming UX regression on long builds | stdlib `subprocess`; west is the existence proof. Cancellation + streaming are explicit acceptance criteria, not assumed. |
| Python environment ambiguity (documented drift trap in this lab) | The shipped artifact bundles its own interpreter; the west venv is resolved explicitly and separately. Never PATH. |
| Startup overhead vs a Rust binary | Measured and recorded in the packaging deliverable. |

## Acceptance criteria

Sub-project 1 is done when all five hold:

1. `tan build` and `tan build --native` run in Python end-to-end on AEN +
   native_sim, producing a real `zephyr.elf` + `zephyr.bin`.
2. Envelope parity with Rust `tan` on every exercised path, success and failure.
3. Execution parity (command / env / skip-fail decision) over the AEN +
   native_sim dry-run matrix.
4. A signed, SHA-256-pinnable single-file artifact on all three platforms, with
   startup overhead recorded.
5. Unit tests green on `core/`; the repo's existing cargo gates still green
   (the Rust workspace is untouched).

## Open question for Hakan

Packaging and verification are his surface. **Does a PyInstaller single-file
artifact satisfy the extension's pinned-hash / signature flow unchanged, or does
the bundling path in `alp-sdk-vscode` need work?** If it needs work, that cost
belongs in this sub-project, not discovered at cutover.
