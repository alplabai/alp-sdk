# WS6-a — `alp.lock` dependency lock (epic #610, Workstream 6)

**Status:** **Implemented, then revised by #1576.** `alp.lock` is generated
on demand -- `west alp-lock` / `scripts/west_commands/alp_lock.py` -- and is
**no longer committed** in the tree: its `digests.metadata` field is a single
hash over the whole `metadata/**` tree, so any two PRs touching different
metadata files rewrote that one committed line from different bases and
conflicted by construction (#1576). `.github/workflows/pr-metadata-validate.yml`
still runs `python3 scripts/west_commands/alp_lock.py --workspace . --check`,
but with no committed lock to diff against, `--check` now only proves the
generator itself still produces a schema-valid, local-path-clean lock -- see
"What `--check` verifies today" below for exactly what that gained up and
gave up relative to the original design.
**Scope:** the *lock format + generator + frozen verify* only. Two other
Workstream-6 threads are explicitly **out of scope** here and become their own
child issues: (b) the one-shot **migration engine** for `board.yaml`/deps/
structure, and (c) the **library-model consolidation** (project-wide vs
per-core `libraries`). This slice only *records* resolved state; it changes no
existing resolution behavior.

## Goal

Give an Alp SDK project a versioned, deterministic, public-safe description of
its exact dependency + toolchain inputs, so an old release can be reproduced
and drift can be detected. Frozen verification fails with an actionable
diagnostic when the live workspace no longer matches the committed lock.

Developer flow:

```
west alp-lock            # writes ./alp.lock for the current workspace
west alp-lock --check    # recompute + schema-validate in memory; nonzero on a
                          # broken generator or a local-path leak (#1576: no
                          # committed ./alp.lock to diff against any more)
```

CI runs `west alp-lock --check`.

## Non-goals (YAGNI)

- No migration engine, no library-model consolidation (separate WS6 children).
- No Yocto-layer or container/toolchain-image identities in v1 (deferred: they
  need bitbake/docker introspection that is environment-dependent and hard to
  test hermetically). The schema reserves an optional `toolchain` object for a
  follow-up but v1 does not populate it.
- No change to any existing resolver, `board.yaml` field, or west manifest.

## Units

Each unit has one purpose, a defined interface, and is testable in isolation.

### 1. `metadata/schemas/alp-lock-v1.schema.json`
Draft 2020-12 JSON Schema, `additionalProperties: false`, `lockVersion: {const: 1}`.
The single authority for the lock shape. Validated by `validate_metadata.py`'s
schema pass (add to the schema-file list) and by the lock's own tests.

### 2. `scripts/alp_lock/__init__.py` — pure builders (no CLI, no argv)
```python
def build_lock(workspace_root: Path, board_yaml: Path | None = None) -> dict
def verify_lock(committed: dict, workspace_root: Path,
                board_yaml: Path | None = None) -> list[Drift]
```
`Drift` is a small dataclass `{path: str, locked, actual}` rendered as one
diagnostic line. `build_lock` composes deterministic sub-collectors:

- `_sdk_identity(workspace_root)` → `{version, revision}`.
  `version` from `scripts/alp_cli/__init__.py __version__`; `revision` from
  `git -C <root> rev-parse HEAD` (via an injectable `rev_resolver` so tests
  don't depend on the real HEAD — default resolver shells `git`).
- `_west_projects(workspace_root)` → sorted `[{name, revision, groups}]` +
  `groupFilter`, parsed from `west.yml` (`yaml.safe_load`, not `west list`, to
  stay hermetic and offline). `revision` is the **manifest-declared pin string
  verbatim** (the sha/tag/branch in `west.yml`), NOT a resolved commit sha —
  resolving would require a live, network-bound west workspace. This locks what
  the manifest *declares*; pinning to resolved shas is a follow-up gated on a
  hermetic `west list` path. Since alp-sdk pins its manifest to fixed
  sha/tag values, this is a real lock in practice; a branch pin is recorded
  as-is (and flagged by the safety guard only if it embeds a path, which it
  never does).
- `_libraries(workspace_root)` → sorted `[{name, version, license, revision}]`
  from `metadata/libraries/*.yaml` (`version`, `license`, and the
  `integration.zephyr.west.revision` pin if present, else `null`).
- `_python_hashes(workspace_root)` → sorted `[{name, version, hash}]` parsed
  from `scripts/requirements.txt`. `hash` is the pinned `--hash=sha256:…` when
  present, else `null`; `version` from the `==` pin, else `null`. No network,
  no install — parse only.
- `_digests(workspace_root)` → `{schemas, metadata}`, each `sha256:<hex>` over
  the canonical concatenation of the relevant files (sorted by repo-relative
  path; each contribution is `path + "\0" + sha256(bytes)`), so the digest is
  order-stable and content-addressed.

`verify_lock` recomputes `build_lock` and returns the field-level drift list
(empty == match). It never writes.

**`sdk.revision` is provenance, not a frozen input.** When the lock lives in the
repo whose HEAD it records, committing the lock advances HEAD past the baked-in
revision, so a frozen check would fail on every later commit. `verify_lock`
therefore records `sdk.revision` (which SDK commit generated the lock) but
excludes it from the drift set (`_PROVENANCE_KEYS`). `sdk.version` plus the west
project pins still lock the SDK identity a consumer builds against.

### 3. `scripts/west_commands/alp_lock.py` — `west alp-lock`
Thin `WestCommand` (mirrors `alp_build` / `alp_renode`): resolves the workspace
root (west topdir, or `--workspace`), optional `--board <board.yaml>`, then:
- default: `build_lock` → write `alp.lock` (sorted keys, `\n`-terminated,
  2-space indent) at the workspace root; print the path.
- `--check` (revised by #1576 -- see "What `--check` verifies today" below):
  `build_lock` in memory, then `jsonschema.validate` it against
  `alp-lock-v1.schema.json`; a `LockError` from `_reject_local` or a
  `jsonschema.ValidationError` exits 1. It no longer loads or diffs against a
  committed `alp.lock` -- there isn't one -- so `verify_lock`/`Drift` are not
  called from this path (they remain the library entry point a lock
  *consumer* uses; see their docstrings in `scripts/alp_lock/__init__.py`).
- Also exposed as a standalone `python scripts/west_commands/alp_lock.py` entry
  (same pattern the other commands use) so headless CI can call it without west.

### 4. Tests — `tests/scripts/test_alp_lock.py` (hermetic)
Build against a **fixture workspace** under `tmp_path` (a minimal `west.yml`,
one `metadata/libraries/*.yaml`, a `requirements.txt`, a couple of schema
files, a stub `__version__`), with a fake `rev_resolver`:
- round-trip: `build_lock` output validates against the schema.
- determinism: two builds byte-identical; list ordering independent of input
  order.
- drift: mutate a west revision / a library version / a schema file → the
  matching `Drift` is reported; unchanged → empty.
- safety guard: assert no value in the lock contains an absolute path or the
  workspace tmp path (the "no local paths" contract); a unit test feeds a
  poisoned input and asserts the guard raises.
- `west alp-lock --check` exits nonzero on drift, zero on match (subprocess).

## Data format (`alp-lock-v1`, camelCase to match `build-plan`)

```json
{
  "lockVersion": 1,
  "generatedBy": "west alp-lock",
  "sdk":     { "version": "0.9.0", "revision": "<git sha>" },
  "west":    {
    "projects": [ { "name": "hal_alif", "revision": "<sha|pin>", "groups": ["hal"] } ],
    "groupFilter": ["-optional"]
  },
  "libraries": [ { "name": "aws-iot", "version": "v3.1.5", "license": "Apache-2.0", "revision": null } ],
  "python":  { "requirements": [ { "name": "jsonschema", "version": "4.21.1", "hash": "sha256:…" } ] },
  "digests": { "schemas": "sha256:…", "metadata": "sha256:…" },
  "resolution": { "board": "<sku|null>", "groupsEnabled": ["hal", "…"] }
}
```

**Determinism & safety invariants (tested):**
- All arrays sorted by `name` (or repo path for digests). No timestamps, no
  `Date`/random (also blocked in scripts). `generatedBy` is a constant.
- Only public inputs: version strings, git SHAs/pins, public URLs are allowed
  but **no absolute/local filesystem paths, no credentials, no private HW
  info**. A `_reject_local(value)` guard enforces this on every leaf string.
- `verify_lock` diff is **field-level and actionable** — names the exact JSON
  path that drifted and both values (the epic's "fail with an actionable
  diagnostic").

## CI wiring

Add a `west alp-lock --check` step to the existing metadata/validate workflow
(path-filtered to the lock inputs: `west.yml`, `metadata/libraries/**`,
`scripts/requirements.txt`, `metadata/schemas/**`, `scripts/alp_cli/__init__.py`).
(Not made a *required* branch-protection context in this slice — promote once
it has soaked, same as other new gates.)

**Superseded by #1576:** this section originally also said to commit the SDK
repo's own `alp.lock` at the repo root as the gate's diff baseline. That was
reverted -- `digests.metadata` is a single hash over the whole `metadata/**`
tree, so every PR touching a different metadata file rewrote that one
committed line from a different base and conflicted with every other open PR
by construction. `alp.lock` is generated on demand and covered by
`.gitignore`; the gate no longer has (or needs) a committed baseline to diff
against -- see "What `--check` verifies today" below.

### What `--check` verifies today (post-#1576)

Still caught: a broken generator (a code change that makes `build_lock`
crash or emit a schema-invalid document) and a local-path/credential leak
(`_reject_local`, e.g. an absolute path landing in a library `license:` or a
west `groups:` entry).

No longer caught by `--check` (this is a real loss, not a wash): a
value-level `build_lock` regression that stays schema-valid -- a narrowed
`_METADATA_DIGEST_GLOBS`, a dropped west project, a digest computed over the
wrong directory (the #1045 bug class). The `digests.metadata` glob-coverage
half of that is still held by a direct test,
`test_alp_lock_metadata_coverage.py` (calling `verify_lock` against a
same-process `build_lock`, not through the CLI); nothing now holds
`_west_projects`/`_libraries` regressing similarly. Nothing was lost on the
original "working tree vs committed lock" axis -- that property is vacuous
once no copy is committed to diff against.

## Acceptance

- `west alp-lock` writes a schema-valid, deterministic `alp.lock` for the SDK
  workspace; re-running is a no-op diff.
- `west alp-lock --check` passes on a matching workspace and fails on a
  broken generator (schema-invalid output) or a local-path leak (proven by
  `test_cli_check_fails_on_schema_invalid_lock` and
  `test_cli_check_fails_on_local_path_leak` in `tests/scripts/test_alp_lock.py`).
  **Superseded by #1576:** the original acceptance criterion here described a
  field-level drift diagnostic against a *committed* `alp.lock` (proven by a
  test mutating a west revision) -- that test, `test_cli_check_fails_on_drift`,
  was deleted when the committed baseline was removed; `verify_lock`'s
  drift-diagnostic behavior is still covered directly (not through the CLI)
  by `test_verify_lock_detects_drift` and the other `verify_lock` tests in
  the same file.
- No local paths / credentials appear in the lock (guard + test).
- Docs: a short "Reproducing a build with alp.lock" section in the docs, and
  Doxygen/README pointers are not needed (no C API in this slice).

## Follow-ups (tracked, not built here)
- WS6-b migration engine (`board.yaml`/deps/structure one-shot migrations).
- WS6-c library-model consolidation (project-wide vs per-core `libraries`).
- Populate the reserved `toolchain` object (Zephyr SDK / container / Yocto
  layer identities) once hermetic introspection exists.
- WS7 build receipt consumes `digests` + the lock digest.
