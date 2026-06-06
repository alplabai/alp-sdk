# Branching and merge policy

Concrete rules for branches, PRs, merges, pushes, and tags in the
Alp SDK repo.  This doc pairs with:

- [`docs/release-policy.md`](release-policy.md) — what gets
  released when (SemVer + LTS).
- [`docs/contribution.md`](contribution.md) — how to file an issue
  or PR.
- [`CODEOWNERS`](../CODEOWNERS) — who must review which paths.

If anything below conflicts with `release-policy.md`'s release-cut
procedure, release-policy wins (it's the version-bearing source).

## The hard rules (TL;DR)

1. **Nobody merges to `main` or `release/*` without a PR.**
   No direct pushes.  Branch protection enforces this server-side.
2. **Every PR requires at least one approving review.**  Including
   from a CODEOWNER for the touched paths.
3. **CODEOWNERS auto-request applies.**  If your PR touches
   `include/alp/`, `metadata/schemas/`, `scripts/alp_project.py`,
   `chips/`, `src/`, `firmware/`, `.github/`, or any other path
   listed in [`CODEOWNERS`](../CODEOWNERS), the matching owner
   **must** approve before merge.  Reviewer assignment is
   automatic on PR open.
4. **All required CI checks must pass.**  See the §"Required CI
   checks" table below.
5. **Force-push to protected branches is forbidden.**  Including
   for repo admins; the branch-protection setting is "Do not
   allow bypassing the above settings".
6. **Self-merging your own PR is forbidden.**  Even if you're the
   maintainer.  The approving review must come from a *different*
   account; if you're the only one on the project, the PR sits
   open until a co-maintainer joins (or until a delegated
   reviewer is invited specifically for that PR).
7. **Conversation threads must be resolved before merge.**  A
   reviewer "Requesting changes" blocks merge until the threads
   they opened are marked resolved.
8. **Signed commits required for `release/*` branches.**  On
   `main` they're recommended but not blocking; on `release/*`
   they're enforced.

These rules apply uniformly to maintainers, contributors, and
admins.  The repo's branch-protection settings are configured so
the GitHub UI's "merge" button is disabled until all gates pass —
there is no "force merge" path available to any human role.

## Branch topology

```
                                  (tagged)         (tagged)
                                  v1.0.0           v1.0.1
                                    │                │
                                    ▼                ▼
release/v1.0 ──────────────────────●────cherry-pick──●──── (LTS, 24-month support)
                                  /                 / ▲
                                 / cherry-pick ↑   /  │
                                /  (sec patches)  /   │ tagged after stabilisation
main ───────────────●─────────────────────●─────────────── (tested, releasable baseline)
                    ▲                       ▲
                    │ dev → main            │ dev → main
                    │ (release gate:        │ (release gate)
                    │  bench/HW/CI passed)  │
dev ──●──────●──────●──────●──────●─────────●───────●─────── (shared integration; untested work)
      ▲      ▲             ▲      ▲                 ▲
      │      │             │      │                 │
   merge  merge         merge  merge             merge
  (--no-ff)(--no-ff)   (--no-ff)(--no-ff)       (--no-ff)
      │      │             │      │                 │
   feat/A  fix/B        feat/C  docs/D           feat/E   ─── (feature branches:
      │      │             │      │                 │           branch off dev,
      └──PR──┘             └──PR──┘                 │           merge back into dev)
                                                   PR
```

Feature branches branch off `dev` and merge back into `dev` via PR
(`--no-ff`, so each merge stays an auditable record).  `dev` is the
shared integration branch for not-yet-tested work.  Promotion of
`dev` → `main` is the **release gate**: it is crossed only after the
integrated work passes testing (bench / hardware / CI).  `main` is
therefore always a tested, releasable baseline, and version tags are
cut on `main`.

### Branches that exist

| Branch                                    | Purpose                                                                 | Direct push?            | Deletion policy                           |
|-------------------------------------------|-------------------------------------------------------------------------|-------------------------|-------------------------------------------|
| `main`                                    | Tested, releasable baseline.  Only `dev` merges in, after testing.      | **Forbidden**.  PR only. | Permanent.                                |
| `dev`                                     | Shared integration branch for untested work.  Feature branches merge here first. | PR only (by convention).  | Permanent.                                |
| `release/v1.0`                            | LTS branch (first LTS).  Lives 24 months from v1.0.0 tag.                | **Forbidden**.  PR only. | Permanent until LTS retires; then archive. |
| `release/v1.1`, `release/v1.2`, ...       | Future LTS branches once promoted.                                       | **Forbidden**.  PR only. | Permanent during their LTS window.         |
| `release/v1.0-rc`, `release/v1.1-rc`, ... | Short-lived release-candidate branches.  Cut from `dev` for stabilisation before the `dev` → `main` promotion + tag. | **Forbidden**.  PR only. | Delete after final tag lands.              |
| `feat/<topic>`, `fix/<topic>`, `docs/<topic>`, ... | Feature / fix / doc branches.  Branch off `dev`, merge back into `dev` via PR (`--no-ff`).  Type prefix per the commit-message `<type>` set. | OK on the contributor's fork or branch. | Auto-deleted on PR merge.            |
| `dependabot/...`                          | Bot-created dependency-bump branches.                                    | Bot push only.          | Auto-deleted on PR merge.                  |
| `pre-1.0-archive/<tag>`                   | Frozen post-mortem snapshots of pre-1.0 tags.  Read-only.                | Read-only.              | Permanent.                                 |

### Branches that explicitly do NOT exist

- `develop` / `next` — `dev` is the single shared integration
  branch; we don't keep additional aliases for it.
- `staging` / `qa` — verification is by Twister + the HiL runs in
  `.github/workflows/nightly-*.yml`, not by a dedicated branch.
  `dev` is the integration branch; the `dev` → `main` promotion is
  the gate that those verification runs guard.
- Long-lived per-contributor `master` clones — fork-based PRs only.

## What lands where

All new work lands on `dev` first (via a feature branch + PR); it
reaches `main` only after testing, when `dev` is promoted at the
release gate.

| Change type                                 | Target branch                       | Required?                             |
|---------------------------------------------|-------------------------------------|---------------------------------------|
| Feature, refactor, ABI addition             | `dev`                               | Always.  Promoted to `main` at the release gate. |
| Bug fix to current pre-release              | `dev`                               | Always (pre-1.0; no LTS branches yet). |
| Bug fix to active LTS (post-1.0)            | `dev` first, then cherry-pick to `release/v<MAJOR>.<MINOR>` | Both — fix lands on `dev` (and reaches `main` at the release gate); the cherry-pick to LTS is the **backport PR**. |
| Critical security fix to active LTS        | `release/v<MAJOR>.<MINOR>` directly, then forward-port to `dev` | Either order; both must land. |
| Doc-only change                             | `dev`                               | Same PR rules as code.                |
| CI workflow change                          | `dev`                               | Requires `@alpCaner` review (CODEOWNERS `/.github/`). |

### Backport flow (post-1.0)

```
1. Fix lands on dev (PR_A, --no-ff merge), and reaches main when
   dev is next promoted at the release gate.
2. Open PR_B against release/v1.0 with the same commit
   cherry-picked.  Title prefix: "[backport v1.0]".
3. PR_B description links PR_A.
4. CI runs the same checks on PR_B as PR_A.
5. Merger is the maintainer who landed PR_A or the LTS owner.
6. After PR_B merges, a v1.0.x tag may follow per release-policy.md
   §"Security backports" / §"Release-cut procedure".
```

## Merge rules

### PR requirements (gated by branch protection)

The table below is gated server-side by branch protection on `main`
and `release/*`.  The same review + CI gates also apply to PRs that
merge a feature branch into `dev` (enforced by convention, since
that is where work first lands); the `dev` → `main` promotion is a
PR like any other and is held to the full `main` gate set.

For both `main` and `release/*` branches:

| Gate                              | Required for `main` | Required for `release/*`    |
|-----------------------------------|---------------------|------------------------------|
| At least 1 approving review       | Yes                 | Yes (LTS owner or maintainer). |
| CODEOWNERS approval               | Yes (auto-requested) | Yes.                        |
| All required CI checks pass       | Yes (see list below) | Yes.                        |
| Branch is up-to-date with target  | Yes                 | Yes.                         |
| Conversations resolved            | Yes                 | Yes.                         |
| Signed commits                    | Recommended; required for `release/*`. | Required.        |
| Force-push to the protected branch | **Forbidden** (admins included). | **Forbidden**.            |
| Direct push (non-PR) bypass        | **Forbidden** (admins included). | **Forbidden**.            |

### Required CI checks

Every PR targeting `dev`, `main`, or `release/*` must pass these
workflows (on `dev` they catch problems before integration; on
`main` / `release/*` they are enforced server-side):

- `pr-static-analysis` — lints, clang-tidy, shellcheck.
- `pr-doxygen` — zero warnings across `include/alp/*.h`.
- `pr-plain-cmake` — non-Zephyr build path stays clean.
- `pr-twister` — Zephyr `native_sim` + matrix builds.
- `pr-metadata-validate` — schema validation on every
  `metadata/**` and `examples/**/board.yaml`.
- `pr-generated-files` — `soc_caps.h` + ABI snapshot in sync.
- `pr-gd32-bridge-build` — firmware tree builds under both
  `BRIDGE_HAL_BACKEND=stub` and `=gd32`.
- `pr-abi-snapshot` — **post-1.0 only**.  Flags removed /
  signature-changed `[ABI-STABLE]` symbols.

Pre-1.0 the `pr-abi-snapshot` gate is informational (allowed to
break the seal alongside a CHANGELOG entry).  Post-1.0 it's a
hard gate.

### Merge method

- **Feature branch → `dev`**: **merge commit with `--no-ff`** by
  default.  The non-fast-forward merge keeps an auditable record of
  every integrated branch on `dev`.  One PR = one feature branch =
  one CHANGELOG entry.  Exception: multi-§-step commit chains (e.g.
  the §C.15a..d HAL bring-up batch where each opcode is a separate
  commit) keep their individual commits via the `--no-ff` merge so
  each step stays bisectable.  The CODEOWNER approving the PR picks
  whether to squash the branch first.
- **`dev` → `main`** (the release gate): **merge commit with
  `--no-ff`**, crossed only after the integrated work passes testing
  (bench / hardware / CI).  This is the promotion that keeps `main`
  a tested, releasable baseline; tags are then cut on `main`.
- **`release/*`**: **cherry-pick from `main` + merge commit**.
  The cherry-pick should keep the original SHA's commit message
  prefixed with `(cherry picked from commit <sha>)` so the
  backport trail is auditable.
- **Never**: plain fast-forward merge that erases the branch
  boundary on `dev` or `main` (creates topology
  noise + makes bisect harder).

### Commit-message style (enforced informally; pre-commit hint)

```
<type>(<scope>): <short summary> (§C.<N>)

<body explaining WHY, not WHAT.  The diff shows WHAT.>

<optional: validators run, expected CI outcome.>
```

`<type>` is one of `feat`, `fix`, `refactor`, `docs`, `test`,
`ci`, `chore`, `perf`.  `<scope>` is the touched subtree
(`gd32-bridge`, `examples`, `ci`, `vendor-sdks`, ...).
The `§C.<N>` tag matches the marker in `CHANGELOG.md` for the
session-of-record (see §C.* convention in CHANGELOG).

**Don't add `Co-Authored-By: Claude` footers.** Per the
maintainer convention, commits attribute solely to the
maintainer (or to whichever human ran the keyboard).

## Push policies

### Maintainers and contributors

- Nobody — including admins — pushes directly to `main` or
  `release/*`.  Branch protection enforces this server-side.
- Fork-based or feature-branch pushes are unrestricted; CI
  runs on push for any branch.
- Force-push to feature branches is fine while the PR is in
  draft; once review starts, prefer additional commits (the
  branch is merged into `dev` with `--no-ff` at merge time).
- Force-push to `main` / `release/*` is forbidden (server-side
  enforced).  If a bad merge lands, **revert via PR** rather
  than force-resetting.

### Tags

- All version tags (`v0.5.0`, `v1.0.0`, `v1.0.1`, ...) are
  **signed** (`git tag -s`).
- Tag creation triggers `.github/workflows/release.yml` (§C.27
  SLSA-L3 release pipeline).
- Once pushed, a tag is **never deleted or re-pointed**.  If a
  tag was created in error, a new tag with a `+1` patch
  number is the only allowed fix.
- Lightweight tags are forbidden — `git tag <name>` without
  `-s` will fail the release pipeline's "tag is signed" check.

### Pushing tags to the wrong branch

Tags are commit-level (not branch-level) in git, but the release
workflow uses the tag's commit's first-parent ancestry to decide
whether the tag was cut from `main` or `release/v<N>`.  If you
accidentally tag from a feature branch, the release pipeline
refuses to publish and a new tag from the correct branch is
required.

## Branch protection settings (GitHub-side)

The settings below are what the maintainer should configure under
Repository Settings → Branches.  Documented here so an audit-trail
exists; the live settings are the authoritative copy.

Server-side branch protection is applied to `main` and `release/*`.
`dev` is the shared integration branch: the PR + review + CI flow
into `dev` is followed by convention (it's where work first lands
and is integrated), and the `dev` → `main` promotion is itself a PR
held to the full `main` gate set below.

### `main`

**Pull request requirements:**

- ✅ Require a pull request before merging
  - ✅ Require approvals: **1** (must be a different account
        than the PR author -- self-approval is forbidden)
  - ✅ Dismiss stale pull request approvals when new commits are pushed
  - ✅ Require review from Code Owners (auto-requested from
        [`CODEOWNERS`](../CODEOWNERS) for the touched paths)
  - ✅ Require approval of the most recent reviewable push (any
        new commit after approval invalidates the approval)
  - ❌ Allow specified actors to bypass required pull requests
        (no bypass list; admins included)

**Status check requirements:**

- ✅ Require status checks to pass before merging
  - ✅ Require branches to be up to date before merging
  - **Required checks** (all must be green):
    - `pr-static-analysis`
    - `pr-doxygen`
    - `pr-plain-cmake`
    - `pr-twister`
    - `pr-metadata-validate`
    - `pr-generated-files`
    - `pr-gd32-bridge-build`

**Merge gates:**

- ✅ Require conversation resolution before merging
- ✅ Require signed commits (recommended; not blocking on `main`)
- ❌ Require linear history (we accept rebase-merge for multi-step
     chains)

**Push restrictions:**

- ✅ Lock branch: **off** (we want PRs to merge)
- ✅ **Do not allow bypassing the above settings** (admins
     included).  This is the load-bearing setting -- it removes
     the "merge anyway" button from the GitHub UI for every role.
- ✅ Restrict who can push to matching branches: **empty list**
     (nobody pushes directly to `main`; merge is via PR only)
- ❌ Allow force pushes (off; admins included)
- ❌ Allow deletions (off; `main` is permanent)

### `release/*`

Same as `main` plus a stricter set:

- ✅ Require signed commits (**required**, not optional)
- ✅ Require approvals: **2** (one of which MUST be `@alpCaner`
     or the LTS-owner-of-record).  Backport PRs to a live LTS
     branch are visible enough that a second pair of eyes is
     cheap insurance.
- ✅ `pr-abi-snapshot` added to required checks (post-1.0)
- ✅ Restrict who can push to matching branches: empty list (PR
     only, same as `main`; the "restricted push list" is
     belt-and-braces, not the primary gate)

### Tags

- ✅ Restrict creation: tags matching `v*.*.*` may be created
  only by `@alpCaner` (and trusted release runners).
- ✅ Restrict updates: existing tags cannot be moved.
- ✅ Restrict deletion: existing tags cannot be deleted.

## Hotfix flow

If a critical fix needs to land on `release/v1.0` before its
corresponding patch can sit on `dev` (and reach `main` at the next
release gate):

```
1. Cut a hotfix branch from release/v1.0:
   git checkout -b fix/hotfix-cve-123 release/v1.0

2. Make the fix.  Cherry-pickable single commit if possible.

3. PR_A → release/v1.0.  All gates required.

4. After PR_A lands + v1.0.x is tagged, forward-port:
   git checkout -b fix/cve-123-forward dev
   git cherry-pick <PR_A's commit>

5. PR_B → dev.  Title prefix "[forward-port from release/v1.0]".

6. PR_B can land normally; CI may have evolved between
   release/v1.0 and dev, so the cherry-pick may need rebases.
   The fix reaches main when dev is next promoted at the
   release gate.
```

Security backports follow this flow plus the embargo procedure
in [`docs/security-advisories.md`](security-advisories.md).

## What to do when things go wrong

### "I pushed to the wrong branch"

If on a feature branch: just fix locally and re-push.  CI
re-runs.

If somehow to `main` (shouldn't be possible with branch
protection): the maintainer reverts via PR.  Force-reset is
**never** the answer.

### "I tagged the wrong commit"

A new tag with `+1` patch number from the correct commit is the
only allowed fix.  The release pipeline ignores the old tag.

### "A merged PR breaks CI on main"

1. Open a revert PR (`git revert <merge-sha>` on a feature
   branch + PR back to `main`).
2. Land it through the normal PR flow.
3. The original author opens a follow-up PR with the fix
   addressing the regression.

Never disable CI or push directly to "fix it quickly" — that
breaks the gate-everything-with-a-PR invariant.

## Cross-reference

- Release cadence + LTS semantics: [`docs/release-policy.md`](release-policy.md).
- Per-version cadence: [`VERSIONS.md`](../VERSIONS.md).
- Security disclosure / embargo: [`docs/security-advisories.md`](security-advisories.md).
- Reviewer assignments: [`CODEOWNERS`](../CODEOWNERS).
- PR template (what each PR description should contain):
  `.github/PULL_REQUEST_TEMPLATE.md` (in the repo root, alongside
  CODEOWNERS).
