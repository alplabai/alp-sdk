### Fixed — the `abidiff` gate no longer skips silently between minors (#2851)

**`scripts/check_abidiff.py` compares against the previous minor's highest release or rc tag, report-only with a `::warning::`, when the current minor has no tag yet (dev right after a minor bump), and fails under `--require-tool` when no release tag exists at all.** The gate stays advisory: it is not a required context and has no `merge_group:` trigger. Tests cover the fallback, `feed-*` tags, rc10 vs rc9 ordering, abidiff exit 1/2 and a cross-minor `--baseline-tag`.
