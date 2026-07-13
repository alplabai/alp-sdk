# Task 5 report — Apply migration #001 across the repo + regen catalog

Branch: `feat/610-alp-migrate` (worktree `/home/caner/alp-ws6b-migrate-wt`)
Commit: `d0ca8f34` — "chore(migrate): stamp schemaVersion:1 on all board.yaml + regen catalog (#610 WS6-b)"

## Step 1 — Preview (sanity)

`python3 scripts/west_commands/alp_migrate.py --preview --all | head -60`

Output showed unified diffs each inserting exactly one line, `+schemaVersion: 1`,
immediately after each file's leading banner comment, plus a `diagnostic-v1`
JSON block per file. First sample (`examples/aen/aen-analog-validate/board.yaml`):

```
--- a/.../aen-analog-validate/board.yaml
+++ b/.../aen-analog-validate/board.yaml
@@ -13,6 +13,7 @@
 # alp-dac0 / alp-adc0 aliases the portable backends resolve.

+schemaVersion: 1
 som:
   sku: E1M-AEN801
```

(A `BrokenPipeError` traceback appeared after `head -60` closed the pipe —
expected, not a bug; all 60 lines shown before it were clean single-line
insertions.)

## Step 2 — Apply

`python3 scripts/west_commands/alp_migrate.py --apply --all --no-verify`
→ 98 `alp-migrate: migrated .../board.yaml` lines.

`git diff --name-only | grep -c '/board.yaml$'` → **98** (matches expected count).

## Step 3 — Drift gate

`python3 scripts/check_board_schema_version.py` →
```
OK: all board.yaml at schemaVersion 1.
```

## Step 4 — Comment-preservation spot-check

`git diff examples/peripheral-io/uart-hello-world/board.yaml`:

```diff
--- a/examples/peripheral-io/uart-hello-world/board.yaml
+++ b/examples/peripheral-io/uart-hello-world/board.yaml
@@ -12,6 +12,7 @@
 # and `west build`.  CMakeLists.txt invokes
 # scripts/alp_project.py at configure time + layers the
 # generated alp.conf over prj.conf via OVERLAY_CONFIG.
+schemaVersion: 1
 som:
   sku: E1M-AEN801
```

Exactly one added line; leading banner comment and all other lines untouched.

## Step 5 — Regen catalog

`python3 scripts/gen_catalog.py` → `wrote metadata/catalog.json  (11 SoMs)`

## Step 6 — Tests

`python3 -m pytest tests/scripts/test_gen_catalog.py tests/scripts/test_board_schema_version.py tests/scripts/test_alp_migrate.py -q`

First run: **1 failed, 30 passed** —
`test_apply_text_adds_exactly_one_line_on_real_board` failed because it read
the real `examples/peripheral-io/uart-hello-world/board.yaml` assuming it was
still unmigrated; since Task 5 permanently stamps that file, `apply_text` on
it is now a no-op (idempotent), so the expected "added line" assertion broke.
This is a pre-existing test bug from Task 4 exposed by Task 5 doing its job —
not a regression in the migration logic itself.

**Fix (in scope, minimal):** updated the test to strip the `schemaVersion: 1`
line from the now-canonical file before feeding it to `apply_text`, so it
still exercises the byte-faithful comment-preserving path against real repo
content, and added an assertion that the result round-trips back to the
canonical (stamped) file. File: `tests/scripts/test_alp_migrate.py`.

Second run: **31 passed** — full green, including the 2 previously-failing
`test_gen_catalog.py` cases now passing (catalog regenerated with the
`check_board_schema_version` gate listed).

## Step 7 — Commit

Critical pre-commit verification:
- `git diff --numstat -- '**/board.yaml'` — every one of the 98 files showed
  `1  0  path` (1 insertion, 0 deletions). No reformatting anywhere.
- Non-board.yaml changes: only `metadata/catalog.json` (regen, expected) and
  `tests/scripts/test_alp_migrate.py` (the stale-test fix above, disclosed
  deviation from the plan's literal file list, required to satisfy Step 6's
  "all PASS").
- Staged selectively (`git add` on the 98 board.yaml + catalog.json + the one
  test file) rather than a blind `git add -A`, to keep pre-existing untracked
  `.superpowers/sdd/{progress,task-1-report,task-3-report}.md` artifacts from
  other tasks out of this commit.

Commit `d0ca8f34`: "chore(migrate): stamp schemaVersion:1 on all board.yaml +
regen catalog (#610 WS6-b)" — 100 files changed, 109 insertions(+), 1
deletion(-) (98 board.yaml x 1 line each = 98, + catalog.json 4 lines net +1
after the 1 deletion in it, + test file +8/-1).

## Summary

- 98 board.yaml files stamped `schemaVersion: 1`, comment/order-preserving,
  zero reformatting.
- `metadata/catalog.json` regenerated (11 SoMs).
- `check_board_schema_version.py` gate green.
- 31/31 tests pass across `test_gen_catalog.py`, `test_board_schema_version.py`,
  `test_alp_migrate.py`.

## Concern

One test in `test_alp_migrate.py` (written in Task 4) hard-coded an assumption
that a real, tracked repo board.yaml would remain unmigrated — that
assumption is permanently false after Task 5. Fixed here; flagging for
Task 6/final-verification awareness in case other tests/docs make the same
assumption about that file's pre-migration byte content.
