### Docs — document rebase-not-merge for long-lived feature branches (#2349)

`gh pr diff` / GitHub's "Files changed" view has been observed to disagree
with the actual branch head when the head is a `Merge origin/dev into
<branch>` commit — a reviewer can be shown a hunk naming symbols that
don't exist at `HEAD` (verified instance: #1839, `src/backends/adc/alif_e7.c`).

Changes:
- `docs/branching-and-merge-policy.md:203` ("Keeping a long-lived feature branch in sync with `dev`")
  adds the missing convention: rebase (or a fresh branch + cherry-pick)
  instead of merging `dev` into a feature branch, plus a verification step
  (`git show <headRefOid>:<path>`) for when a merge commit at the head is
  unavoidable.

No code change: the underlying stale-diff behaviour is GitHub/`gh`-side
rendering, not something alp-sdk's own tooling produces or can patch. A
CI guard that diffs `gh pr diff` against `git diff $(merge-base) HEAD`
was suggested in the issue as an optional follow-up; adding a new
`check_*.py` gate for it is a separate maintainer decision, not folded
into this fix.
