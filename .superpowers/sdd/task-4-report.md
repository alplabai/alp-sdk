# Task 4 report — west alp-migrate command

STATUS: DONE
Commit: 0e4cfbbf9dadc93341db34320d1d9b06f5f1c834 — "feat(migrate): west alp-migrate check/preview/apply CLI (#610 WS6-b)"

## Files
- `tests/scripts/test_alp_migrate.py` — appended `_load_cli()` + `test_cli_apply_stamps_file` + `test_cli_check_nonzero_on_drift` (Step 1). Confirmed fail first (`FileNotFoundError` on missing `alp_migrate.py`), then pass after Step 3.
- `scripts/west_commands/alp_migrate.py` — new, mirrors `alp_lock.py` structure (`WestCommand` subclass `AlpMigrate` + standalone `main()`); `run()`'s apply path is the single line `new_text, report = alp_migrate.apply_text(text)` per instructions (not `apply(doc)`+`dump()`).
- `scripts/west-commands.yml` — registered `alp-migrate` entry immediately after `alp-lock`.

## Test summary
`python3 -m pytest tests/scripts/test_alp_migrate.py -v` → 12 passed (10 prior + 2 new CLI tests).
Full `tests/scripts/` suite: 1497 passed, 21 skipped; 2 pre-existing failures in `test_gen_catalog.py` (catalog out of sync with Task 3's already-committed `check_board_schema_version.py` gate) — verified via `git stash` these fail identically at HEAD before this change; they are Task 5's responsibility (regen catalog), out of scope here.

## Concern
The plan's literal Step-1 CLI test (`test_cli_apply_stamps_file`) asserts `out.startswith("schemaVersion: 1")` for input `"# hi\nsom:\n  sku: X\n"`. That assertion was written against the pre-review `apply(doc)`+`dump()` form. The now-mandated `apply_text` (Task 2's byte-faithful writer, already committed/reviewed) deliberately preserves a leading whole-line comment above the stamp — so for that input it produces `"# hi\nschemaVersion: 1\nsom:\n  sku: X\n"`, not byte-0 `schemaVersion`. This matches `apply_text`'s documented contract and the already-passing `test_apply_text_adds_exactly_one_line_on_real_board`/idempotent tests, and matches the real repo board.yaml banner-preservation behavior Task 5 relies on. I judged the `startswith` assertion itself stale rather than the engine wrong (engine is out of Task 4's scope and already reviewed), and narrowed that one assertion to `assert "schemaVersion: 1" in out` (kept the `"# hi" in out` comment-preservation check, added a one-line comment explaining why). Everything else in the two CLI tests is verbatim from the plan. Flagging this in case the plan's canonical test text should be corrected upstream.

## Fix report — review findings (2026-07-13)

STATUS: DONE
Commit: (see below) — "fix(migrate): require a mode, catch MigrateError, propagate regen failure (#610 WS6-b)"

### Findings fixed
1. **Bare-invocation default was destructive.** `_add_args` had no required mode; a plain `west alp-migrate` fell through to `run()`'s tail (`args.check` false, `getattr(args, "apply", False)` false) and did nothing visible, but a bare invocation with no flags at all previously risked landing on default `_targets()` (`Path("board.yaml")` in cwd) with ambiguous intent. Fixed by making `--check`/`--preview`/`--apply` a `required=True` mutually exclusive group, and `--all`/`--board` a (non-required) mutually exclusive group so both can't be given together. `argparse` now raises `SystemExit` on a bare invocation instead of silently doing nothing or writing.
2. **`run()` swallowed `alp_migrate.MigrateError`.** `alp_migrate.load()`/`plan()`/`apply_text()` can raise `MigrateError` (e.g. downgrade refused) with no `try` around them — an uncaught exception would print a Python traceback to the user instead of a clean CLI message. Wrapped both the `load`/`plan` call and the `apply_text` call in `try/except alp_migrate.MigrateError`, printing `alp-migrate: {path}: {e}` and returning 1.
3. **`--apply` guard used `getattr(args, "apply", False)`** defensively instead of relying on the now-mandatory arg; simplified to `args.apply` since the mode group guarantees the attribute exists and exactly one mode is set. Also changed the per-path write from an unconditional `# --apply` fallthrough to an explicit `if args.apply:` check (matches the three-way mode split now that modes are mutually exclusive).
4. **`_verify()` silently discarded `gen_catalog.py`'s exit code** (`subprocess.run(..., check=False)` then `return 0` unconditionally) — an apply could report success while the catalog regen actually failed. Fixed to capture the `CompletedProcess`, print a stderr message and propagate `cp.returncode` on failure.
5. **Docstring oversold `--apply`** ("rewrite in place, regen, run pr profile") — no "pr profile" step exists. Corrected the third usage line to `west alp-migrate --apply      # rewrite in place + regen derived files`.

### Files
- `scripts/west_commands/alp_migrate.py` — `_add_args` (mandatory mode group + exclusive `--all`/`--board`), `run()` (catch `MigrateError`, guard write behind `args.apply`), `_verify()` (propagate regen failure), module docstring (drop non-existent "pr profile" claim).
- `tests/scripts/test_alp_migrate.py` — appended `test_cli_requires_a_mode` (bare invocation raises `SystemExit`, file untouched) and `test_cli_migrate_error_is_clean` (`--check` on a schemaVersion-999 board.yaml returns 1 with a clean `alp-migrate:` stderr message, no traceback).

### Test summary
`python3 -m pytest tests/scripts/test_alp_migrate.py -v` → 14 passed (12 prior + 2 new).
Full `tests/scripts/` suite: 1499 passed, 21 skipped, 48 subtests passed; the only failures are the 2 pre-existing `test_gen_catalog.py` failures (Task 5's scope, unrelated to this fix, confirmed identical before/after this change).

### Concern
None — all five review findings map to a concrete, isolated code change with a passing regression test; no scope creep beyond `alp_migrate.py` + its test file.
