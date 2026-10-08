@page docs_release_notes_index Release notes

# `docs/release-notes/` — one file per released version

Add the release's GitHub Release body as a **new file in this directory**,
committed *before* the tag is pushed -- not hand-assembled and pasted in
after the fact.

## Why

Before this convention (alp-sdk#1728), the lean body was drafted into a
local scratch file and applied with `gh release edit vX.Y.Z --notes-file
<lean.md>` *after* `release.yml` had already published the Release page
with the raw `CHANGELOG.md` slice as its body -- exhaustive by
construction (a full `[Unreleased]` section runs tens to hundreds of
thousands of characters; `v0.16.0-rc1`'s was 255,413). That gap lasts
however long it takes a human to notice the page is live and go paste the
real body in.

A file committed here lands through the same PR as the version bump and
the CHANGELOG slice (`docs/release-policy.md`'s release-cut procedure,
step 0), so `release.yml` reads it directly and publishes the lean body
from the first second the Release page exists. See that workflow's
"Prefer the committed release-notes file for the release body" step.

## How

Create `docs/release-notes/v<MAJOR.MINOR.PATCH>.md` -- the exact
version the tag will carry (`v0.16.0`, `v0.16.1`, ...; every release
gets its own file, patch releases included). The file's content is the
release body **exactly as it should appear** on the GitHub Release page:
nothing is rewrapped or summarised further downstream.

Shape (see `v0.16.0` or later releases at
<https://github.com/alplabai/alp-sdk/releases> for real examples):

```
*One or two sentences: what this release is about, in plain language.*

## Highlights

- **Bold-led, one-sentence summary of the change.** A second sentence only
  if the reader needs it to act (a migration, a config change).
- **Another highlight.** (#1234)

## Breaking

(only if there is one -- omit the section otherwise)

- **What broke, bold-led.**
  - Sub-bullet: what a consumer must do about it. Sub-bullets are for
    breaking / ABI / security items only -- everything else stays a
    single bullet.

## Verifying this release

The source tarball carries a SLSA L3 in-toto provenance attestation:

    gh attestation verify <downloaded-tarball> --repo alplabai/alp-sdk

---

Full detail: [CHANGELOG.md](../blob/main/CHANGELOG.md) · [full diff](../compare/v<PREV>...v<N>)
```

`CHANGELOG.md` stays the exhaustive record (every entry, every issue
number); this file is the summary a reader sees first.

## Computing "what changed since the last release" -- read this before drafting

**Diff `main`'s own tag history, never `dev`, and never `git merge-base
main dev`.** Compute it as `git log v<PREV>..v<N>`, run on `main` once
`dev` has been promoted to `main` for the cut in progress -- NOT the
footer's `compare/v<PREV>...v<N>` link above, which is GitHub's
three-dot merge-base comparison, a different computation. That range
is always intact, because `dev` -> `main` promotion is a real `--no-ff`
merge commit -- it never goes through `dev`'s merge queue. Do NOT add
`--first-parent`: on `main` the first-parent chain is only the
promotion merges themselves (3 commits between v0.15.0 and v0.16.0);
the release's real work hangs off their *second* parents, so
`--first-parent` drops it (162 commits without it, for that same
range).

The opposite direction is broken by design and will silently re-list
work a prior release already shipped: the mandatory post-tag back-merge
(`main` -> `dev`, so `dev`'s `metadata/sdk_version.yaml` doesn't stay
stale) targets `dev`, and **every** merge into `dev` goes through the
repo's "dev merge queue" branch ruleset, whose `merge_method` is
`SQUASH` unconditionally -- confirmed live via `gh api
repos/alplabai/alp-sdk/rulesets`. A squash-merged back-merge is a single
new commit on `dev`; neither `main`'s tip nor the tag that triggered it
becomes an ancestor of `dev`. `git merge-base main dev` after any
back-merge therefore resolves to whatever commit the two branches last
shared *real* ancestry at -- a point that predates every release cut
since -- so `git log <that merge-base>..dev` (or any diff that assumes
`main` is reachable from `dev`) walks all the way back through releases
that already shipped and reports their contents as new. This is not a
bug to fix by trying to force a non-squash back-merge PR: the merge
queue's `SQUASH` method is server-enforced ruleset config, not a PR-time
choice, and applies to every PR merged into `dev`, back-merge included.
See `docs/release-policy.md`'s "Back-merge `main` -> `dev`" section and
`docs/adr/0029-cross-repo-pins-are-typed-lock-entries-with-a-property-gate.md`
(observation 9 / clause 4) for the fuller record of this behaviour.
