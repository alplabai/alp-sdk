# 0014. The `alp` CLI consumes the orchestrator's emitted build plan

Status: Accepted
Date: 2026-06-04
Deciders: alpCaner (alp-sdk), Hakan (alp-sdk-vscode)

## Context

The alp-sdk-vscode repo ships the native `alp` CLI + IDE extension
("Wave C" on their roadmap: the CLI sits at the top of the build and
invokes `west` / `bitbake` / `cmake` directly, instead of shelling out
to `west alp-build`).  Three proposals were exchanged:

1. **Shared Rust crate** — extract `alp_orchestrate.py` into an
   `alp-build-core` crate both repos consume.  Rejected: forces a
   rewrite of working Python, couples the repos' release trains.
2. **Rust planner mirror** — the CLI re-implements the planner
   (board.yaml → slices, IPC/DTS/sysbuild/TF-M derivation) in Rust,
   with `alp_orchestrate.py` demoted to a parity-test golden.
   Rejected: the planner is the fast-moving, vendor-heavy half (it
   doubled — 1547 → 3066 lines — in the three v0.6 weeks); mirroring
   it is a standing re-implementation tax and a second source of
   planner truth, violating the SDK's one-machine-readable-source
   rule.  The prize ("no Python at runtime") is small: `west` and
   `bitbake` are Python, so every build host has a Python-bearing
   SDK checkout anyway.
3. **Emitted build plan** (this ADR) — the SDK adds
   `alp_orchestrate.py --emit build-plan`; the CLI consumes the JSON.

The settlement is recorded on their side in
`alp-sdk-vscode/docs/PROPOSAL-alp-build-core.md` (2026-06-04).

## Decision

`scripts/alp_orchestrate.py` grows `--emit build-plan`: deterministic,
write-free, schema-versioned JSON carrying everything the consumer
needs to materialise + run the build itself.

The split: the **SDK owns the planner** (slice resolution, partition
allocation, IPC/DTS/sysbuild/TF-M derivation, generated-file
*contents*) and the builders; the **CLI owns the mechanism** —
materialise (pure byte-write of the plan's files), execute (run each
slice's command), schedule (parallelism + incremental cache), progress
UX, JSON envelope.

Contract properties (locked with the consumer):

- **camelCase keys; independent `schemaVersion`** — bumped on breaking
  shape changes, flagged in the CHANGELOG.
- **Artefacts carry `contents`** (`GeneratedFile {path, contents}`)
  for both `sharedArtefacts` and per-slice `configArtefacts`, so the
  consumer's materialise step is pure IO and no content-derivation
  logic leaks out of the SDK.  Internally, `_shared_artefacts` /
  `_slice_config_artefact` are the single sources both the emit and
  the Orchestrator's own materialise step read — the plan and the
  on-disk build cannot drift by construction.
- **No `inputHash`** (the consumer computes its own cache key over the
  plan) and **no `sequential`** (parallelism policy belongs to the
  consumer's scheduler).
- **One slice per non-`off` core**, sorted by `coreId`.  A slice the
  script cannot build yet (e.g. a TBD Zephyr board target) is carried
  with `command: null` plus a `no-command` warning — never dropped.
- The per-slice `command` shape is **not frozen** — it will grow (e.g.
  `--sysbuild --sysbuild-config` when the Phase 3 conf→build wiring
  lands); consumers track release tags and re-baseline on CHANGELOG
  notice.
- **Additive-change rule:** a new top-level key, a new per-slice
  sub-object, or a new key inside an existing sub-object never bumps
  `schemaVersion` on its own — only removing, renaming, or changing the
  meaning of an existing key does.  Additive keys/sub-objects still get a
  CHANGELOG entry so consumers know to look for them.  First exercised
  by the #610 §4 per-slice `toolchain` / `artifacts` / `debug` tooling
  index (see Amendments below).

## Consequences

- The SDK commits to: `board.yaml` schema-version bumps on breaking
  changes; `schema_version` retained on `metadata/e1m_modules` YAMLs;
  CHANGELOG heads-up for changes to the per-slice command shape,
  build-dir convention, env keys, or metadata layout; answering the
  conf→build wiring question before the CLI's first end-to-end phase.
- Consumers pin to **release tags**, never `dev` — the emit must ship
  in a tagged release to be consumable.
- `west alp-build` **stays native** (the shim-over-`alp build` idea is
  withdrawn): standalone `west` usage is a first-class consumer path,
  and an SDK west command must not depend on a binary from another
  repo.
- Wiring `emit_sysbuild_conf` into the shared-artefact set (it was
  emit-only before) means a `boot:` block now also materialises
  `build/alp_sysbuild.conf` during `west alp-build` — previously the
  overlay was only available via `--emit`.

## Amendments

### 2026-07-18 — envelope provenance keys + tooling-index camelCase fix

- **Added two additive envelope keys** (per the additive-change rule
  above, no `schemaVersion` bump): `sdkVersion` (the `version:` field
  from `metadata/sdk_version.yaml` at emit time) and `sdkCommit` (short
  git commit of the emitting checkout, `git rev-parse --short HEAD`;
  `null` when git or the `.git` directory isn't available — the emit
  never fails on this).  Lets a cached or materialised plan be traced
  back to the exact planner revision that produced it.
- **Corrected the #610 §4 per-slice tooling index to camelCase.**
  `toolchain.target_triple` and `artifacts.size_report` /
  `artifacts.compile_commands` had landed in snake_case, violating this
  ADR's own "camelCase keys" contract property.  Renamed to
  `toolchain.targetTriple` / `artifacts.sizeReport` /
  `artifacts.compileCommands`.  #610 §4 shipped with no CHANGELOG entry
  documenting the exact field shape, so this is treated as a pre-release
  correction rather than a breaking change: no known consumer parses
  these sub-keys yet (alp-sdk-vscode's cli-rs deliberately models only
  the core slice; alp-studio does not parse the tooling index).

### 2026-07-18 — `executionPolicy` envelope key

- **Added one additive envelope key** (per the additive-change rule
  above, no `schemaVersion` bump): `executionPolicy`
  (`{"unknownBackend": "fail", "missingTool": "skip", "nullCommand":
  "skip"}`), publishing the skip-vs-fail rules
  `Orchestrator._dispatch_slice` itself applies so a plan consumer
  stops hand-porting them. `executionPolicy` is **always emitted**
  (strict producer) but is **optional** in the schema (tolerant
  consumer), so a field-absent plan — e.g. a `v0.11.1` plan — still
  validates and a consumer applies the documented default. (Corrected
  2026-07-20: it was briefly `required`, which is a breaking shape
  change at unchanged `schemaVersion 1`; reverted per the ADR-0020
  amendment. The consumer pins `schemaVersion == 1`, so a bump was not
  the fix.)
