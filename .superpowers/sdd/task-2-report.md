# Task 2 Report — scripts/alp_migrate/: pure engine + registry #001 + report

**Status:** DONE
**Branch:** feat/610-alp-migrate (worktree /home/caner/alp-ws6b-migrate-wt)
**Commit:** fcb4f5f4 — "feat(migrate): pure board.yaml migration engine + registry #001 (#610 WS6-b)"

## What changed

- `tests/scripts/test_alp_migrate.py` (new) — the 8 tests from Step 1 of the
  plan verbatim, with `# SPDX-License-Identifier: Apache-2.0` prepended per
  repo convention (the plan's code block omitted it for this file only).
- `scripts/alp_migrate/migrations/m000_to_v1.py` (new) — migration #001
  (`FROM=None, TO=1`). Body differs from the plan's literal listing (see
  Concerns): the plan's `doc.insert(0, "schemaVersion", TO)` alone leaves a
  leading file-level comment (`doc.ca.comment`, which ruamel attaches to the
  *map*, not a specific key) rendered above the newly-inserted key, failing
  `test_apply_stamps_and_preserves_comments`'s `out.startswith("schemaVersion: 1")`
  assertion. Fixed by re-homing that leading comment onto the original first
  key before inserting, so it renders in its original place (now just below
  `schemaVersion`) instead of above it. `FROM`/`TO`/`transform(doc, report)`
  signature and the report message (`"m000_to_v1: stamped schemaVersion: 1"`)
  are unchanged from the plan.
- `scripts/alp_migrate/migrations/__init__.py` (new) — `STEPS` registry,
  exact code from the plan.
- `scripts/alp_migrate/__init__.py` (new) — engine (`LATEST`,
  `current_version`, `plan`, `apply`, `load`, `dump`, `diff`,
  `report_to_diagnostics`, `MigrateError`, `Report`), exact code from the plan.

## Commands run + output

1. Wrote `tests/scripts/test_alp_migrate.py` (Step 1).
2. `python3 -m pytest tests/scripts/test_alp_migrate.py -v` →
   **1 error during collection**, `ModuleNotFoundError: No module named
   'alp_migrate'` — confirmed the expected pre-implementation failure (Step 2).
3. Created the 3 source files per Steps 3-5, using the plan's code verbatim.
4. `python3 -m pytest tests/scripts/test_alp_migrate.py -v` →
   **7 passed, 1 failed** (`test_apply_stamps_and_preserves_comments`):
   `out.startswith("schemaVersion: 1")` was `False` because ruamel's
   map-level leading comment stayed pinned above the mapping regardless of
   which key was inserted at position 0. Verified interactively
   (`doc.ca.comment` vs `doc.ca.items`) before patching `transform`.
5. Patched only `m000_to_v1.transform` to move the leading comment onto the
   pre-insertion first key's item-comment slot, then insert `schemaVersion`.
   Re-ran: `python3 -m pytest tests/scripts/test_alp_migrate.py -v` →
   **8 passed** (Step 6, matches the plan's expected count).
6. Spot-checked against a real repo file
   (`examples/peripheral-io/uart-hello-world/board.yaml`) via `alp_migrate.diff`:
   the multi-line leading `# board.yaml -- ...` comment block renders
   unchanged, with only `+schemaVersion: 1` added above it — matches Task 5's
   documented expectation.
7. Full regression sweep: `python3 -m pytest tests/scripts/ -q` →
   **1493 passed, 21 skipped** (pre-existing, unrelated).
8. `git add scripts/alp_migrate tests/scripts/test_alp_migrate.py`; confirmed
   via `git status --short` that only these 4 files were staged (untracked
   `.superpowers/sdd/progress.md` / `task-1-report.md` left alone).
9. Committed with the exact Step 7 message. No `Co-Authored-By` / AI
   attribution.

## Test pass counts

- `tests/scripts/test_alp_migrate.py`: 8/8 passed.
- Full `tests/scripts/` regression sweep: 1493 passed, 21 skipped (no
  regressions).

## Scope notes

Per the plan, `check_board_schema_version.py` (Task 3), the `west
alp-migrate` CLI (Task 4), the repo-wide stamping + catalog regen (Task 5),
and docs/CHANGELOG (Task 6) are out of scope here and untouched.

## Concerns

- **One line of code deviates from the plan's literal listing** (in
  `m000_to_v1.transform`): the plan's exact `doc.insert(0, "schemaVersion",
  TO)` body does not pass the plan's own Step 1 test as written — ruamel
  attaches a leading file-level comment to the map itself, not the
  soon-to-be-second key, so it still renders above the inserted
  `schemaVersion` line. I fixed this within the same function (re-home the
  comment before inserting) rather than relaxing the test, since the public
  interfaces (`FROM`, `TO`, `transform(doc, report)` signature, report
  message) that Tasks 3/4 depend on are unchanged and all 8 specified tests
  now pass exactly as written. Flagging so whoever owns the plan doc can
  reconcile the listed code with this fix before Task 5's repo-wide apply.
- Separately (not fixed, not in scope): applying the engine to a real
  board.yaml also reformats existing flow-sequence items (e.g. `pins: [{
  e1m: ... }]` loses its inner spacing, and `- uart` list-item indentation
  shifts) because the plan's `_yaml()` doesn't pin `y.indent(...)` to match
  the repo's existing style. This will add noise beyond the single
  `+schemaVersion: 1` line Task 5 expects on some files — worth a look before
  Task 5's repo-wide apply, but outside this task's file list.

## Follow-up fix (WS6-b review blocker)

**Status:** DONE
**Commit:** (this fix) — "fix(migrate): byte-faithful apply_text write path (#610 WS6-b)"

The second Concern above was confirmed as a real review blocker: dumping a
whole `board.yaml` through the ruamel round-trip doc reformats real files
(loses block-sequence indent on 94/98 repo files; drops flow-map inner
padding `{ e1m: X }` -> `{e1m:X}` on 38/98). The file-write path for the
repo-wide stamping step (Task 5) must not do that.

Two changes to `scripts/alp_migrate/__init__.py`:

- `_yaml()`: added `y.indent(mapping=2, sequence=4, offset=2)` to match the
  repo's actual block-sequence style. This narrows (but does not eliminate)
  doc-level reformatting for callers that still use `load`+`apply`+`dump`
  for programmatic/diagnostic use.
- Added `_leading_comment_end()` + `apply_text(text) -> (str, Report)`: a
  byte-faithful line-editor. It still parses/plans via the existing
  round-trip `load`/`plan` (to reuse version-detection and the
  downgrade-refusal error), but instead of re-serializing the whole doc, it
  finds the first non-comment, non-blank line of the raw text and inserts
  `schemaVersion: 1\n` immediately above it — leaving every other byte of
  the file untouched. Only the `None -> 1` adoption step has a text
  strategy; any other planned step raises `MigrateError` (there is no other
  step registered yet, so this is dormant until a #2 migration lands).
  `dump`/`apply` (doc-level) are left in place for callers that need a
  parsed-doc round trip rather than a file write.
- `tests/scripts/test_alp_migrate.py`: added
  `test_apply_text_adds_exactly_one_line_on_real_board` and
  `test_apply_text_idempotent_real_board`, both against the real
  `examples/peripheral-io/uart-hello-world/board.yaml` (which has both an
  indented `- ` sequence and a `{ e1m: ..., macro: ..., doc: ... }` flow
  entry with inner padding). The first proves the only line-level diff is
  the added `schemaVersion: 1`; the second proves running it twice is a
  no-op.

### Commands run + output

- `python3 -m pytest tests/scripts/test_alp_migrate.py -v` -> **10 passed**
  (the pre-existing 8, including `test_apply_stamps_and_preserves_comments`
  unchanged and still passing, plus the 2 new byte-faithfulness tests).
- `python3 -m pytest tests/scripts/ -q` -> **1495 passed, 21 skipped**
  (skips pre-existing/unrelated; count is +2 over the prior 1493 for the new
  tests, no regressions).
- `git status --short` / `git diff --stat` before commit: only
  `scripts/alp_migrate/__init__.py` and `tests/scripts/test_alp_migrate.py`
  modified (59 insertions, 0 deletions); no other files touched.

### Test pass counts

- `tests/scripts/test_alp_migrate.py`: 10/10 passed.
- Full `tests/scripts/` regression sweep: 1495 passed, 21 skipped (no
  regressions; delta is exactly the 2 new tests).

### Concerns

- `apply_text` only has a text strategy for the `None -> 1` step (by
  design, per the task). Whoever implements a future structural migration
  (a `1 -> 2` step, etc.) must add its own line-level text strategy to
  `apply_text` or it will hard-fail with `MigrateError("no text writer for
  migration step ...")` rather than silently falling back to the
  reformatting doc-level `dump`. Flagging so that isn't a surprise later.
- The doc-level `load`/`apply`/`dump` path is still reformatting-prone for
  any caller that uses it directly on a full file (unchanged from the first
  Concern above, just narrowed by the indent pin) — Task 5's repo-wide
  stamping script must call `apply_text` on the raw file text, not
  `dump(apply(load(text)))`, to get byte-faithful output.
