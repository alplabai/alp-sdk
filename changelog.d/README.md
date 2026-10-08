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

Create `changelog.d/<issue>.md`, where `<issue>` is the GitHub issue number
the entry belongs to.

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

`<slug>` is lowercase alphanumeric segments joined by single hyphens — short,
descriptive, no leading/trailing/doubled hyphen (matches
`^\d+(-[a-z0-9]+(-[a-z0-9]+)*)?\.md$`).

```
changelog.d/1909-diagnostic-format-uri.md
```

**Ordering.** `assemble_changelog.py` sorts fragments by leading issue
number, then by the full filename as a tie-breaker, so `<issue>.md` always
sorts before `<issue>-<slug>.md` for the same issue — the plain filename is
a strict prefix of the suffixed one, so it compares as "less". Two suffixed
fragments for the same issue sort against each other by slug text.

**Enforced join key.** The leading digits must match the issue number this
fragment's own `### ... (#N)` heading cites, keeping them the join key back
to that issue for `assemble_changelog.py`'s sort order and for anyone
grepping `changelog.d/` by number. `scripts/check_changelog_fragment_issue.py`
(alp-sdk#1957) checks the two agree wherever a heading cites exactly one
issue number, or cites several and the filename's leading digits are outside
both the cited set and the span between the smallest and largest of them
(e.g. citing `#1848, #1814` from a `1940.md` filename); a heading citing a
range or several issues that the filename's leading digits DO fall inside
(e.g. `#1757-#1783` from a `1761.md` filename), or no `(#N)` at all, is
outside what that check can decide and is left to hand review — the suffix
only breaks the filename tie.

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

## Citing code

Every new `` `path:line` `` citation must carry an anchor: a short verbatim
quote from the cited lines, in parentheses of its own, right after it.

```
the guard at `scripts/foo.py:42` ("if not fragments:")
```

`scripts/check_changelog_citations.py` is **text-authoritative,
line-advisory** for a citation that already existed before your change: if
the quote is not in the cited range, it searches the whole cited file for the
same text before failing. Found in exactly one other place, AND long enough
(16+ characters — a short, generic quote like `"rc=$?"` doesn't qualify, see
the script's `_MIN_DRIFT_ANCHOR_LEN`), AND re-anchorable there without
running past the end of the file, the citation still PASSES — the claim
held, only the line moved — with an informational note telling you to
re-run `--fix`; found nowhere, found more than once, or too
short/would-overflow, it FAILS. So an unrelated line inserted or removed
above a PRE-EXISTING citation no longer breaks it by itself.

**A citation you are adding or editing right now gets none of that
tolerance.** If your diff touches the line, the gate requires the anchor AND
the line to already be correct — there is no unrelated-shift history for it
to forgive, and a coincidental match elsewhere would be exactly the wrong
thing to trust silently.

The quote goes AFTER the citation — `` (`scripts/foo.py:42`, "if not
fragments:") `` looks anchored and is rejected as a near miss — and stays on
one markdown line. For a range, quote its FIRST line: `--fix` restarts the
range at the line the anchor is found on, so `:10-14` anchored on line 13
becomes `:16-20` after a 3-line shift, not `:13-17`. `--fix` is a tidy-up,
not a requirement for a pre-existing citation — it re-derives the stored
line number from the anchor so a future duplicate stays resolvable by
uniqueness; `--against-merge` grades the merge before you make it. When
`--fix` re-homes a quote shorter than the 16-character floor, it still
rewrites it but marks the rewrite `[VERIFY: ...]` and counts it in its
summary: read each such line before committing, because a short quote can
be unique only because the occurrence it described was deleted. Better
still, lengthen the quote. The script's docstring has the full rules.

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

`bump_version.py` also refuses the bump outright if
`check_changelog_citations.py --strict-lines` fails against `[Unreleased]` —
a citation that is merely advisory everywhere else must already be correct
right before it freezes into a released section, because a released section
is never rewritten by `--fix` afterward. If it fails, run `python3
scripts/check_changelog_citations.py --fix`, review the rewrite, commit it,
and re-run the bump.

## Do not migrate old entries

The 47 entries already merged under `## [Unreleased]` before this system
landed stay exactly where they are — they're already merged and conflict with
nothing. `changelog.d/` is for new entries only.

## Editing `CHANGELOG.md` directly

Still correct for: fixing a typo in a shipped entry, correcting an already-
released section, or any edit that is not "a new entry for unreleased work".
The fragment rule exists to stop many PRs racing one insertion point — it is
not a ban on ever touching the file.
