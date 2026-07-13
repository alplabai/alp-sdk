# WS6-b — `alp-migrate` board.yaml migration engine (epic #610, Workstream 6)

**Status:** design approved 2026-07-13; **revised 2026-07-13 → lazy versioning
(see note).**

> **Revision — lazy versioning (supersedes the "stamp all 98" decision below).**
> `schemaVersion` stays an explicit board.yaml key, but is **lazy**: an absent
> key IS version 1 (the floor), permanently. There is **no** adoption/stamp
> migration (#001 removed) and the repo's board.yaml are **not** mass-stamped —
> the field only appears once a real v1→v2 migration bumps a file. The engine,
> `west alp-migrate` CLI, and `check_board_schema_version.py` gate all still
> ship (the gate is a clean no-op until a v2 lands). The engine's byte-faithful
> apply is proven by a synthetic v1→v2 migration in the tests. Sections below
> that describe stamping all 98 files / migration #001 are retained for history
> but superseded by this note.

**Scope:** the *versioned, one-shot migration engine* for `board.yaml` +
`board.yaml` **schema versioning**, plus its first real migration
(**#001 unversioned → v1**). Builds directly on WS6-a (#729), which named this
slice as a follow-up. The library-model consolidation (WS6-c) and any
dependency-declaration / project-structure migrations remain out of scope and
become later registry entries (#002+).

## Goal

Give an Alp SDK project a versioned, comment-preserving, one-shot way to bring
its `board.yaml` up to the current schema, with a preview/diff and a
machine-readable report, so schema evolution never silently strands a project
on an old shape. No runtime ABI shims, no deprecated aliases — the file is
rewritten to the new shape (per §6 + the repo's no-legacy-compat rule).

Developer flow:

```
west alp-migrate --check      # report each board.yaml's version; nonzero on drift
west alp-migrate --preview    # unified diff + diagnostic-v1 JSON, no writes
west alp-migrate --apply      # rewrite in place, regen derived files, run pr profile
```

CI runs `python3 scripts/check_board_schema_version.py` (fast, all files).

## Non-goals (YAGNI)

- No library-model consolidation (WS6-c, separate child).
- No dependency-declaration or generated-project-structure migrations in this
  slice — the registry is built to hold them, but #001 is the only step shipped.
- No C-API call-site rewriting/reporting: #001 is a pure `board.yaml`
  transform with no C surface, so the "report call sites needing manual work"
  requirement is designed into the report schema but exercised by a later
  migration, not #001.
- No new resolver behavior. Read stays lenient: a `board.yaml` with **no**
  `schemaVersion` is read as v1, so external/handwritten projects that never
  run the migration keep loading unchanged.

## Units

Each unit has one purpose, a defined interface, and is testable in isolation.

### 1. `metadata/schemas/board.schema.json` — add `schemaVersion`

Add an optional top-level `schemaVersion` (integer, `minimum: 1`). Two
distinct semantics, deliberately kept separate:

- **Read/resolver leniency:** a `board.yaml` with no `schemaVersion` loads as
  v1 (documented in the schema `description`; enforced by the read path). This
  is what keeps external/handwritten projects working unchanged.
- **Canonical form (migration/gate):** the canonical, migrated shape carries
  the field *explicitly*. A missing field therefore counts as "needs the
  adoption migration," even though it *reads* as v1. This split is what lets
  #001 stamp the field without it being a semantic version bump.

No other board.yaml field changes in this slice.

### 2. `scripts/alp_migrate/__init__.py` — pure engine (no CLI, no argv)

Pure functions, no IO beyond reads. ruamel.yaml round-trip mode preserves
comments + key order.

- `current_version(doc) -> int | None` — the *explicit* `schemaVersion`, or
  `None` when absent. (Read-path leniency treating `None` as 1 lives in the
  resolver, not here — the engine must distinguish "absent" from "explicit 1"
  to know a file still needs the adoption stamp.)
- `LATEST: int` — the current schema version (1 after this slice).
- `plan(doc) -> list[Step]` — the ordered steps to reach canonical `LATEST`:
  `None` → the adoption step (#001, stamp `schemaVersion: 1`), then any
  `k → k+1` up to `LATEST`. Empty only when the field is explicitly `LATEST`.
  A newer-than-`LATEST` explicit version raises `MigrateError` (no downgrade).
- `apply(doc) -> (new_doc, Report)` — runs each planned step's transform on the
  round-trip doc, returns the rewritten doc + a `Report` (per-step edits,
  `needs_manual` items). Idempotent: re-running on a current doc is a no-op.
- `diff(old_text, new_text) -> str` — unified diff for `--preview`.
- `MigrateError` — raised on an un-migratable input (unknown/newer version =
  refuse to downgrade; malformed doc).

### 3. `scripts/alp_migrate/migrations/` — the registry

One module per step, keyed `(N, N+1)`, each exposing `transform(doc, report)`.
Seeded with **`m000_to_v1.py`**: sets explicit `schemaVersion: 1` at the top of
the mapping (comment-preserving). Adding a future v1→v2 is a new ~20-line
module + one registry line.

### 4. `scripts/west_commands/alp_migrate.py` — `west alp-migrate`

Thin CLI over the engine: walks the project's `board.yaml` (or repo-wide with
`--all`), dispatches `--check` / `--preview` / `--apply`. `--apply` then calls
the existing regen entrypoint for derived files and runs the `pr` quality
profile on affected slices. Registered in `scripts/west-commands.yml` next to
`alp-lock`.

### 5. `scripts/check_board_schema_version.py` — drift gate

Fails if any repo `board.yaml` is not at `LATEST` (i.e. `plan(doc)` non-empty).
Fast, filesystem-only, runs in CI. Mirrors the other `check_*.py` gates;
auto-listed in `catalog.json` via `gen_catalog.py`.

### 6. Migration #001 applied to the repo

Run `--apply` to stamp explicit `schemaVersion: 1` into all 98 repo
`board.yaml` files (comment-preserving), committed in this PR so repo files are
canonical and the drift gate is green.

### 7. Tests — `tests/scripts/test_alp_migrate.py` (hermetic)

- round-trip fixture: comments + key order survive a stamp.
- idempotency: `apply` twice == `apply` once.
- `needs_manual` reporting surfaces in the `Report`.
- downgrade (explicit version > `LATEST`) refusal raises `MigrateError`.
- `current_version` returns `None` when absent (not 1) — the read-leniency
  "absent loads as v1" is tested at the resolver, not the engine.
- the drift gate: a fixture missing `schemaVersion` fails (plan non-empty); an
  explicitly-stamped `LATEST` one passes.

## Data format — migration report (reuses `diagnostic-v1`)

`--preview`/`--apply` emit a JSON report per file, each entry a `diagnostic-v1`
record (uri, range, severity, code `alp.migrate.*`, message, hint) so existing
diagnostic consumers (§4) render it with no new schema. `needs_manual` items
are `severity: warning` diagnostics pointing at the exact location.

## CI wiring

- `check_board_schema_version.py` added to the gate set (branch-protection
  optional; auto-listed in `catalog.json`).
- `--apply` path: regen derived files, then `pr` profile + a native_sim twister
  build of affected examples to prove stamped files still build.

## Acceptance

- [ ] `board.schema.json` carries optional `schemaVersion`; absent reads as v1.
- [ ] Engine `plan`/`apply`/`diff` + registry with #001, ruamel round-trip.
- [ ] `west alp-migrate --check/--preview/--apply` wired + registered.
- [ ] All 98 repo `board.yaml` stamped to v1; drift gate green.
- [ ] Hermetic pytest covers round-trip, idempotency, refusal, gate.
- [ ] `catalog.json` regenerated (new gate + docstring).
- [ ] native_sim build of affected examples green post-stamp.

## Follow-ups (tracked, not built here)

- WS6-c library-model consolidation (its own migration entry #002+).
- Dependency-declaration + project-structure migrations.
- C-API call-site manual-work reporting, exercised by a migration that needs it.
