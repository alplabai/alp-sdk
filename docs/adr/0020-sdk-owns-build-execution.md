# 0020. The SDK plans; a standalone `tan` CLI is the whole command surface (three repos, one executor)

Status: Accepted — direction + alp-sdk Phase 1/4 code implemented on `dev`; a
release-blocking remediation is outstanding (see **Amendment** below).
Date: 2026-07-18 (Caner) · 2026-07-20 (Hakan co-sign, this commit)
Deciders: alpCaner (alp-sdk), Hakan (alp-sdk-vscode)
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

## Amendment (2026-07-20 — Hakan co-sign + corrections)

Hakan ratifies the **direction** (end-state B) and the alp-sdk-side Phase 1/4
code. Three points below correct or condition the record; the release train is
blocked until the remediation is met. Tracked in #855.

1. **The completeness gate this ADR mandates is not yet in place.** *The one thing
   that must hold* + *Cross-repo oracle trigger* require an automatic
   `repository_dispatch` from alp-sdk CI into `tan`'s build-validation on every
   planner change, plus the two-seam comparator, **before** the irreversible
   Phase 4. That trigger does not exist in `.github/workflows` yet — Phase 4 code
   (fan_out deletion, #848) landed ahead of it. This is recoverable, not a
   rollback: `df312cec^` (`97ad481b`) still carries both `fan_out` and the Phase-1
   fields, so the oracle is reconstructed retroactively. **Remediation (blocks any
   release/tag): freeze that oracle, stand up the two-seam gate + the cross-repo
   trigger, then tag.** (Verified: the only `97ad481b`↔`df312cec` emit delta is
   `debug.probe` `"openocd"→null`, hand-reviewed.)

2. **"No `alp`-named command survives" is narrower than shipped.** Retired: the six
   build verbs + `fan_out`. Surviving on `dev` **by design**: the `--emit`
   planner surface, `west alp-migrate/alp-lock/alp-quality/alp-emit`, and the
   Python `alp` console script with 11 non-build verbs
   (`generate/validate/init/doctor/run/model/monitor/new_som/faultdecode/explain/emit`),
   which `tan` forwards to. `docs/cli.md` already documents this real end-state;
   §Decision-1/§Open-Q-4 overstate it. No surviving SDK verb shells out to `tan`
   (dependency stays one-way, tan→SDK).

3. **Contract fix owed to v0.12 (schema-`required` at unchanged version).** `#847`
   made `executionPolicy` `required` in `build-plan-v1.schema.json` while
   `schemaVersion` stayed `const: 1` — a breaking shape change without a bump.
   Since the consumer pins `schemaVersion == 1`, do **not** bump to 2 (it would
   strand `tan`); instead revert the two fields to **optional**, keep the emitter
   always emitting explicit values (strict-producer / tolerant-consumer). `tan`
   already honors + defaults both. This is the version-skew guard (§Decision-5)
   applied correctly.

## Addendum (2026-07-20 — planner ALSO wires per-core config for `tan`; the CMakeLists.txt bridge to non-`tan` consumers is kept, `--core`-scoped)

A `configArtefacts` gap survived Phase 4: every Zephyr slice's plan already
carried its own per-core-scoped `alp.conf` (materialised at
`build/<core>-zephyr/alp.conf`, `buildplan.py::_slice_config_artefact` ->
`kconfig.py::_slice_alp_conf`), but the plan's own `command` never referenced
it — the artefact was DEAD for a `tan`-driven build. Separately, each
example's own `CMakeLists.txt` shelled `alp_project.py --emit zephyr-conf`
at CMake-configure time with **no `--core`**, which sums every core's
symbols into one fragment (`alp_project.py`'s documented unscoped-sum
behavior) — a cross-core Kconfig leak on any multi-Zephyr-core SoM (every
AEN-family example is one: the SoM preset always defaults the unused M55
core to the `alp-stock-shim` Zephyr slice, so a single-app example like
`i2s-tone` silently pulled a second core's symbols in too).

**Decision (Fable advise, Option B, corrected after a merge-blocking review
finding): the planner ADDITIONALLY wires per-core config for `tan`-driven
builds; the CMakeLists.txt bridge is KEPT, not removed, and is `--core`-
scoped to fix the actual leak.** A first pass at this addendum removed the
CMakeLists.txt block outright, reasoning the planner's `EXTRA_CONF_FILE` made
it redundant — that was wrong: `EXTRA_CONF_FILE` only reaches a build routed
through a consumer of `--emit build-plan` (`tan`). `twister`
(`pr-twister.yml`, a **required branch-protection context**) and any bare,
manual `west build` invoke `CMakeLists.txt` directly and never read the plan;
removing the block would have shipped 65 examples with **zero** ALP Kconfig
under twister/bare-`west build` — several have an intentionally empty
`prj.conf` (e.g. `iot-fleet-ota/prj.conf`: *"the effective build config is
the overlay alp.conf emitted... at configure time"*) that depends on
`CONFIG_ALP_SDK=y` from `alp.conf`'s baseline fragment just to pull the SDK
sources into the build — a required-CI-context break, caught before merge.

The corrected, landed shape — **two paths, one shared emitter, byte-parity
pinned**:

1. **`_slice_command` (`orchestrator.py`)** appends
   `-DEXTRA_CONF_FILE=<abs path to the slice's own build/<core>-zephyr/
   alp.conf>` to every Zephyr slice's `west build` command, inside the
   existing `--` passthrough segment (never a second `--`; the deprecated
   `OVERLAY_CONFIG` is not used). The path is resolved absolute and anchored
   on the project's own `board.yaml` directory (`base_dir`), never the
   emitting process's CWD — the same #596 invariant
   `_resolve_app_path`/`_zephyr_app_dir` already honor. This is what a
   `tan`-driven build now uses.
2. **Every example `CMakeLists.txt` that carried the leaky, unscoped emit
   (65 unique files — the audit is `board.yaml`-driven: any project
   resolving 2+ Zephyr cores, cross-referenced against `alp_project.py`'s own
   "no `--core` sums across cores" behavior, not a hand list) keeps its
   `alp_project.py --emit zephyr-conf` call, now `--core`-scoped** to the one
   Zephyr core that CMakeLists.txt actually builds (matching the convention
   the per-core multicore subdirs — e.g.
   `examples/multicore/rpmsg-aen/m55_hp/CMakeLists.txt` — already used). This
   fixes the actual leak (the cross-core sum) while keeping Kconfig reachable
   for twister and any bare `west build`. An additional ~27 examples that
   were never leaky (a single Zephyr core, so the unscoped sum was a no-op)
   are scoped too while touching this code, closing a latent leak for the
   day any of those presets grows a second Zephyr core.
3. **`_emit_library_hw_backends` (per-`libraries:` HW-accelerator backend
   Kconfig, e.g. `CONFIG_ALP_CMSIS_DSP_HELIUM=y`) is folded INTO
   `kconfig.py::_slice_alp_conf` itself** (previously appended only by
   `alp_project.py`'s emit loop, so the planner's `configArtefacts` was
   silently missing it for any example whose `libraries:` declares a
   `hw_backends` matcher — 26 examples had real, non-empty content here).
   Both paths now call the exact same `_slice_alp_conf(project, slice_)` and
   get byte-identical output; `alp_project.py`'s `--core <id>` path emits it
   verbatim (no `# --- core: ... ---` section marker, which stays only for
   the unscoped sum-across-cores case, where multiple sections still need
   telling apart).
4. **`scripts/check_zephyr_conf_parity.py`** (new; mirrors the
   `check_emit_snapshots.py` byte-identical-emit precedent) walks every
   `--core`-scoped example `CMakeLists.txt` under `examples/**`, runs
   `alp_project.py --emit zephyr-conf --core <id>`, and asserts it is
   byte-identical to `_slice_alp_conf(project, project.cores[<id>])` — the
   exact fragment the planner's `configArtefacts` materialises for that same
   core. This is what makes "the two paths coexist idempotently" a checked
   invariant instead of an assertion: a future change to either call site
   (or to `_emit_library_hw_backends`) that forks them fails this gate
   immediately, in CI, rather than surfacing as silently-wrong firmware on a
   bench weeks later.

This landed as ONE atomic change (planner + `_slice_alp_conf` +
`alp_project.py` + every affected example's `--core` scoping + the new
parity gate + regenerated `check_emit_snapshots.py` goldens): double-applying
both paths without folding `_emit_library_hw_backends` into the shared
emitter would have resolved conflicting symbols by undocumented CMake
`-D`/merge-order (last-`-D`-wins on a literal duplicate `-D`, since
`EXTRA_CONF_FILE`'s `alp.conf` and the `OVERLAY_CONFIG`-layered
CMakeLists.txt `alp.conf` would otherwise have carried different content for
the same core); landing the CMakeLists.txt `--core` scoping without the
`EXTRA_CONF_FILE` wiring would have left the plan's `configArtefacts` dead
again for `tan`. Both ship together, byte-identical, pinned.

**Retiring the CMakeLists.txt bridge remains future work, explicitly NOT
done here.** The end-state this ADR's main body describes (`tan` as the sole
executor, alp-sdk as plans-only) still implies the CMakeLists.txt-level emit
eventually goes away — but only once twister (and any other non-`tan`
consumer) has its OWN way to get a `--core`-scoped `alp.conf` into a bare
`west build` (e.g. a twister `extra_args`-driven pre-generation step, or an
equivalent in-tree mechanism). That is a **separate, future ADR**, gated on
that replacement existing; do not delete the bridge again before it does.

`examples/connectivity/iot-fleet-ota/testcase.yaml`'s `native_sim.conf`-
loads-after-`alp.conf` ordering comment is restored to accurate: with the
CMakeLists.txt block kept (now `--core m55_hp`-scoped), `alp.conf` reaches
this twister build again via `OVERLAY_CONFIG`, and `native_sim.conf`'s
`EXTRA_CONF_FILE=native_sim.conf` extra_args still loads after it.

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
