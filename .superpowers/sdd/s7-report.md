# #610 §7 slice 1 — build-receipt-v1 — implementation report

STATUS: DONE

## Commits
- Task 1: `d95f58d6` — feat(release): build-receipt-v1 schema + composer (#610 §7)
- Task 2: `84bf92fa` — feat(ci): check_build_receipt schema gate + catalog/lock/changelog (#610 §7)

## Files
- `metadata/schemas/build-receipt-v1.schema.json` — new closed Draft-2020-12 schema.
- `scripts/build_receipt.py` — pure composer (sha256 hashing, git rev resolver, schema validation).
- `scripts/check_build_receipt.py` — gate asserting the schema is a valid closed schema.
- `tests/scripts/test_build_receipt.py` — 6 tests (5 composer + 1 gate).
- `metadata/catalog.json` — regenerated (`gen_catalog.py`), picked up new gate's docstring; verified idempotent (second regen byte-identical).
- `alp.lock` — regenerated (`alp_lock.py`), new schema file shifted the `schemas` digest; `sdk.revision` provenance key updated too (excluded from drift by design).
- `CHANGELOG.md` — new "Added — build-receipt-v1" entry under `## [Unreleased]`.

## Test summary
`python3 -m pytest tests/scripts/test_build_receipt.py -v` → 6 passed.

## Final verification (all green)
- `python3 -m pytest tests/scripts/test_build_receipt.py -v` — 6 passed
- `python3 scripts/check_build_receipt.py` — OK
- `python3 scripts/west_commands/alp_lock.py --check` — matches
- `gen_catalog.py` re-run — catalog.json unchanged (in sync)
- `git status` — clean working tree after both commits

## Concerns
None. Both TDD cycles went red→green exactly as specified; no deviation from the plan's exact code was needed.

---

# Review-finding fixup — deterministic paths, fail-loud git status, docs

## Commit
- `fix(release): deterministic repo-relative paths, fail-loud git status, receipt docs (#610 §7)`

## Files
- `scripts/build_receipt.py` — added `_relpath()`; `config.boardYaml` and each
  image `path` now repo-relative (posix, bare name if outside root) instead
  of the verbatim caller path, restoring "identical inputs → identical
  receipt" across checkouts. `_git_rev` now checks the `git status` return
  code: rev-parse failure/no-git still degrades to `(None, False)`, but a
  `status` command failing while `git` itself works raises `RuntimeError`
  instead of silently reporting `dirty=False`.
- `tests/scripts/test_build_receipt.py` — added `test_paths_are_repo_relative`
  (7th test).
- `docs/heterogeneous-builds.md` — new "Build receipts (`build-receipt-v1`)"
  subsection under §6 Building, next to the existing build-plan-v1
  paragraph: what the receipt is, its top-level fields, that it's a pure
  composer over existing inputs, no timestamp, and that `release.yml`
  wiring + SBOM/attestation refs are later §7 slices.

## Test summary
`python3 -m pytest tests/scripts/test_build_receipt.py -v` → 7 passed.

## Final verification (all green)
- `python3 -m pytest tests/scripts/test_build_receipt.py -v` — 7 passed
- `python3 scripts/check_build_receipt.py` — OK
- `python3 scripts/check_doc_drift.py` — OK
- `python3 scripts/west_commands/alp_lock.py --check` — matches (schema
  unchanged, so alp.lock stays valid)

## Concerns
None.
