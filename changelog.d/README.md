# `changelog.d/` — one file per change

Add your changelog entry as a **new file in this directory**, not by editing
`CHANGELOG.md`.

## Why

`CHANGELOG.md` has exactly one insertion point — the top of the
`## [Unreleased] - vX candidate` section. Every open PR appends its entry
there, so **any two PRs conflict on it by construction**, and the conflict
re-fires on every merge: land one PR and the rest go dirty again.

Measured 2026-08-12 across the PRs open at the time (alp-sdk#1395): three of
four blocked PRs were blocked by `CHANGELOG.md` alone, with no other
conflicted file. They also conflicted with *each other*, so they could only
be landed one at a time, each cycle gated by a full local CI run.

Disjoint files cannot conflict. One file per change removes the entire class.

## How

Create `changelog.d/<issue>.md`, where `<issue>` is the GitHub issue or PR
number the entry belongs to.

```
changelog.d/1358.md
changelog.d/1366.md
changelog.d/1379.md
```

### A second fragment for the same issue

An issue can legitimately be closed by more than one PR — a tiered fix, a
split, or two independent defects filed under one number. If
`changelog.d/<issue>.md` is already taken, do **not** overwrite it, append
into it (that just trades the file conflict for a merge conflict), or rename
your fragment after the PR instead of the issue. Add a disambiguating suffix
and keep the issue number leading:

```
changelog.d/<issue>-<slug>.md
```

`<slug>` is lowercase `[a-z0-9-]+` — short, hyphenated, descriptive (matches
`^\d+(-[a-z0-9-]+)?\.md$`). The leading digits stay the join key back to the
issue for `assemble_changelog.py`'s sort order and for anyone grepping
`changelog.d/` by number; the suffix only breaks the filename tie.

```
changelog.d/1909-diagnostic-format-uri.md
```

The file's content is **the entry exactly as it should appear** in
`CHANGELOG.md`, starting with its own heading line:

```
### Fixed — `flash_args` carries no `slot0_load_address`, so tan refused to auto-sign an AEN Flow D flash (tan-cli#353)

`flash_args` is emitted by `--emit build-plan` for every AEN target, but
...
```

Unlike `Keep a Changelog`'s six fixed section headings, every alp-sdk entry
carries **its own** `### <Category> — <Title>` heading rather than sharing a
bucketed list — so a fragment is a complete, self-contained block: heading
plus prose, nothing more to wire up. `<Category>` is free text (`Added`,
`Changed`, `Fixed`, `Removed`, `Decided`, `Documented`, `Notes`, `Schema`, or
a new one if none of those fit — there is no enum to keep in sync).

**Bodies are copied byte-for-byte.** The assembler never rewraps, reformats,
or summarises a fragment's text. This changelog carries registers, hex, bit
fields, addresses, SKUs, hw_rev, diagnostic codes, error strings and paths
verbatim — a "helpful" rewrap can silently corrupt one of those. Write the
entry exactly as it should ship.

## Release time

`scripts/assemble_changelog.py` folds every fragment into `CHANGELOG.md`'s
`## [Unreleased] - vX candidate` section, in deterministic (filename/issue)
order, then deletes the fragments:

```sh
python3 scripts/assemble_changelog.py                 # fold + delete fragments
python3 scripts/assemble_changelog.py --check          # list what's pending; change nothing
python3 scripts/assemble_changelog.py --dry-run        # print the result to stdout; write nothing
python3 scripts/assemble_changelog.py --require-empty  # exit 1 if any fragment is still unfolded
```

This runs **before** `scripts/bump_version.py` slices `[Unreleased]` into a
dated `## [vX]` section — `bump_version.py` itself refuses to slice while
`changelog.d/` still holds fragments, so a skipped assemble step fails loudly
at release time instead of silently dropping entries from the release.

## Do not migrate old entries

The 47 entries already merged under `## [Unreleased]` before this system
landed stay exactly where they are — they're already merged and conflict with
nothing. `changelog.d/` is for new entries only.

## Editing `CHANGELOG.md` directly

Still correct for: fixing a typo in a shipped entry, correcting an already-
released section, or any edit that is not "a new entry for unreleased work".
The fragment rule exists to stop many PRs racing one insertion point — it is
not a ban on ever touching the file.
