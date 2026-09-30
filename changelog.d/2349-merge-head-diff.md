### Docs — how to check a PR whose head is a `dev` merge (#2349)

With a `Merge origin/dev into <branch>` commit at the head, GitHub's
rendered PR diff has been seen to disagree with the tree at the head
(#1839, `src/backends/adc/alif_e7.c`). Merging `dev` into feature branches
stays the convention; `docs/branching-and-merge-policy.md` now documents
checking a reviewed hunk against `git show <headRefOid>:<path>` and the
merge-base diff before acting on it. The stale rendering itself is
GitHub-side, not something alp-sdk tooling produces.
