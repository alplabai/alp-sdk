# CI runner architecture — heavy/self-hosted jobs

`alp-sdk` is a **public** repository. GitHub's own guidance is to never
attach a self-hosted runner to a public repo: a fork pull request would
run untrusted code on your hardware. The Yocto **bitbake** build needs
a self-hosted runner (a Yocto host with the licensed RZ BSP). Real-silicon
hardware-in-the-loop (HiL) verification does **not** — it is never CI at
all, self-hosted or otherwise; see [`HW-IN-LOOP.md`](HW-IN-LOOP.md).

We resolve this with a **dispatch bridge**: the public repo never hosts a
runner. It dispatches the heavy work to the **private** `alp-sdk-internal`
repo, whose runners do the build and report the result back as a commit
status on the originating PR.

```
 alp-sdk (PUBLIC)                         alp-sdk-internal (PRIVATE)
 ┌───────────────────────────┐           ┌──────────────────────────────┐
 │ pr-bitbake.yml (bridge)    │  repo_    │ bitbake.yml                  │
 │  • GitHub-hosted runner    │  dispatch │  • self-hosted runner        │
 │  • set pending status      │ ────────► │    [self-hosted,linux,x64,   │
 │  • dispatch (internal      │           │     alp-bitbake]             │
 │    events only; forks      │           │  • checkout alp-sdk@<sha>    │
 │    skipped — no secrets)   │           │  • bitbake per MACHINE       │
 │                            │ ◄──────── │  • POST commit status ───────┼─► shows on the PR
 └───────────────────────────┘  statuses │                              │
                                          └──────────────────────────────┘
```

## Why this is safe

- **No self-hosted runner is ever attached to the public repo.** Fork PRs
  cannot reach your hardware — not "mitigated", eliminated.
- The bridge dispatches **only for internal events** (same-repo PRs,
  pushes, manual dispatch). A fork PR is skipped by the guard, and GitHub
  withholds secrets from fork PRs anyway, so the dispatch token is absent.
- The two tokens are **narrowly scoped** (see below). The dispatch token's
  worst case if leaked is "trigger a build" — no code execution, no host
  access.
- The **red-X still lands on the PR**: the per-MACHINE commit statuses
  (`bitbake · <MACHINE>`) post back, and can be made **required checks**
  in branch protection so a broken Yocto build blocks the dev→main merge.

## Components

| Where | File | Role |
|-------|------|------|
| `alp-sdk` (public) | `.github/workflows/pr-bitbake.yml` | GitHub-hosted bridge: pending status + `repository_dispatch` to alp-sdk-internal |
| `alp-sdk-internal` (private) | `.github/workflows/bitbake.yml` | self-hosted build matrix + status-back |

## Auth: one org-owned GitHub App (short-lived tokens)

Both workflows mint a **short-lived (~1 h) installation token at runtime**
via [`actions/create-github-app-token`](https://github.com/actions/create-github-app-token),
from a single org-owned GitHub App. No long-lived PAT lives in either
repo — the only persistent secret is the App's private key (org-owned,
revocable), and fork PRs never receive it. This is preferred over PATs:
it isn't tied to a person, the runtime token is short-lived and re-scoped
per job, and the App can be revoked centrally.

**App** (`alp-ci-bridge`, owned by the `alplabai` org):

- Repository permissions: **Contents: read & write** (for
  `repository_dispatch` to alp-sdk-internal) + **Commit statuses: read &
  write** (to post status to alp-sdk) + Metadata: read (mandatory).
- Webhook: **disabled** (the App is used only for token minting in Actions).
- Installed on **both** `alp-sdk` and `alp-sdk-internal`.

**Secrets** (the same App, set on both repos):

| Secret | Repos | Value |
|--------|-------|-------|
| `ALP_CI_APP_ID` | alp-sdk + alp-sdk-internal | the App's numeric App ID |
| `ALP_CI_APP_PRIVATE_KEY` | alp-sdk + alp-sdk-internal | the App's downloaded `.pem` private key |

```bash
gh secret set ALP_CI_APP_ID          --repo alplabai/alp-sdk           --body "<APP_ID>"
gh secret set ALP_CI_APP_PRIVATE_KEY --repo alplabai/alp-sdk           < app-private-key.pem
gh secret set ALP_CI_APP_ID          --repo alplabai/alp-sdk-internal  --body "<APP_ID>"
gh secret set ALP_CI_APP_PRIVATE_KEY --repo alplabai/alp-sdk-internal  < app-private-key.pem
```

Each workflow scopes its minted token to just the repo it touches: the
bridge → `repositories: alp-sdk-internal` (dispatch); the build →
`repositories: alp-sdk` (status).

## Self-hosted runner (on alp-sdk-internal)

Labels must match the private workflow's `runs-on`:

| Job | Labels | Host |
|-----|--------|------|
| bitbake (Yocto) | `self-hosted, linux, x64, alp-bitbake` | i9 Ubuntu box w/ the RZ BSP v6.30 tree |

Bitbake is the only self-hosted job today. There is no AEN (or any
other) HiL runner, on this repo or on `alp-sdk-internal` — on-silicon
verification is a manually-invoked bench run instead, not CI at all;
see [`HW-IN-LOOP.md`](HW-IN-LOOP.md) for why a HiL runner (public or
private) is unlawful/unworkable here, not just unbuilt.

Register against the **private** repo:

```bash
# token: gh api -X POST repos/alplabai/alp-sdk-internal/actions/runners/registration-token --jq .token
./config.sh --unattended --url https://github.com/alplabai/alp-sdk-internal \
  --token <TOKEN> --name i9-alp-bitbake \
  --labels self-hosted,linux,x64,alp-bitbake --work _work
```

Runner environment (in `~/actions-runner/.env`):

- `ALP_POKY_ROOT` → RZ BSP v6.30 poky tree (poky + meta-renesas +
  meta-rz-features/* + meta-deepx/imx/graphics). May live inside
  alp-sdk-internal alongside the other license-gated vendor files.
- `ALP_SSTATE_ROOT`, `ALP_DL_ROOT` → persistent sstate / downloads dirs.

Run **ephemeral or containerized** (fresh per job) for defense-in-depth.

## Untrusted-input handling in `run:` blocks

GitHub Actions substitutes every `${{ }}` expression into a step's `run:`
block **as source text, before the shell parses it** — not as a shell
argument. If the expression's value is attacker-influencable and contains
shell metacharacters (`$()`, backticks, `;`, `&`, `|` — all legal in, e.g., a
git branch name per `git-check-ref-format`), that text executes as code on
the runner. This is a template injection, not a shell-quoting bug, so
quoting inside the `run:` script does not help — the payload is already
part of the script by the time bash sees it (alp-sdk#1475).

**Rule:** never splice one of the following contexts directly into a `run:`
block via `${{ }}`. Route it through the step's `env:` block instead, and
reference it as a quoted shell variable (`"$THE_VAR"`). This is the complete
list — it is the `_ATTACKER_CONTEXTS` tuple in
`tests/scripts/test_workflows_are_loadable.py`, and the two must stay
identical:

- `github.event.pull_request` (especially `.head.ref`, `.head.sha`,
  `.title`, `.body`)
- `github.event.issue`
- `github.event.comment`
- `github.event.review`
- `github.event.head_commit`
- `github.event.commits`
- `github.event.inputs`
- `github.event.workflow_run`
- `github.head_ref`
- `github.ref_name`
- `github.ref`
- `github.actor`

Matching is substring containment, so each entry covers its whole subtree
(`github.event.pull_request` covers `.head.ref`, and `github.ref` also
covers `github.ref_name`/`.ref_type`/`.ref_protected`).

```yaml
# Wrong -- payload substituted into the script before bash parses it:
run: echo "${{ github.event.pull_request.head.ref }}"

# Right -- the value is only ever *data*:
env:
  HEAD_REF: ${{ github.event.pull_request.head.ref }}
run: echo "$HEAD_REF"
```

`actions/github-script` is the same sink with a different interpreter: its
`with: script:` body is JavaScript that GitHub substitutes `${{ }}` into
before Node parses it. Same rule, same `env:` indirection — the script reads
the value back through `process.env` instead of a shell variable:

```yaml
# Wrong -- payload substituted into the script before Node parses it:
uses: actions/github-script@3a2844b7e9c422d3c10d287c895573f7108da1b3  # v9
with:
  script: core.info("${{ github.event.pull_request.title }}")

# Right:
uses: actions/github-script@3a2844b7e9c422d3c10d287c895573f7108da1b3  # v9
env:
  PR_TITLE: ${{ github.event.pull_request.title }}
with:
  script: core.info(process.env.PR_TITLE)
```

This has to be applied **per step**, not once per workflow: `${{ }}` is
re-substituted independently into every `run:` block, so quoting a value at
the step that first receives it does not protect a later step that reads it
back out of `steps.<id>.outputs.*` and splices it again. Each consuming step
needs its own `env:` indirection.

A step output carries no trust of its own. `steps.<id>.outputs.*` is only as
trustworthy as the expression that assigned it, so a `${{ }}` reference in a
`run:` body must be judged on that **root**, not on the fact that it names a
`steps.*` value. `release.yml` is the worked example: its `Parse tag + verify
against metadata` step published `GITHUB_REF_NAME` verbatim as
`steps.tag.outputs.tag`, validating
only the part before the first `-`, and three later steps spliced that output
into their `run:` source text. Nothing in those three bodies named a context
at all.

`tests/scripts/test_workflows_are_loadable.py` checks this: it fails if a
`run:` block interpolates one of the contexts above directly. Three limits on
how much that check is worth:

- **It matches direct context references only.** The check is substring
  containment against the text of each `${{ }}` expression, so it cannot see
  a value that reaches a `run:` body transitively through
  `steps.<id>.outputs.*` — the `release.yml` case above passed it green.
  Tracing a step output back to its root is a **manual** step; do it by hand
  whenever a `run:` body reads one, and do not treat a green run as having
  done it for you. (Covering it mechanically needs data-flow analysis across
  steps rather than substring matching, and the check does not attempt that.)
- **It is advisory, not blocking.** On a workflow-only edit the sole job
  running `pytest tests/scripts/` is `cross-platform-zephyr`'s
  `python-smoke` (no other workflow's `paths:` filter matches
  `.github/workflows/**`), and `python-smoke` is in neither branch's
  required-status-check list. `main` requires exactly
  `twister · native_sim/native/64` and `clang-format · diff-only`;
  `dev` requires exactly `twister-shard 1/4`, `twister-shard 2/4`,
  `twister-shard 3/4`, `twister-shard 4/4`, `clang-format · diff-only`
  and `distro install · all`. A violation therefore posts a red,
  non-required check; it does not block the merge.
- **It covers the two direct source-text sinks, and nothing else.** `run:`
  bodies and `actions/github-script` `with: script:` bodies (alp-sdk#1529)
  are both walked. A `with: script:` on any *other* action is an input
  string that action receives rather than JavaScript it evals, so it is
  deliberately not checked; a composite action's own `run:` steps live
  outside `.github/workflows/` and are likewise unchecked.

## Third-party action pinning

Every `uses:` in `.github/workflows/` must resolve a 40-character commit
SHA, not a mutable tag — a retagged or compromised upstream release must not
change what a workflow executes without a new PR pinning it forward
(alp-sdk#1479). Keep the tag as a trailing comment so a reviewer does not
have to resolve the SHA back to a human-readable version by hand:

```yaml
uses: actions/checkout@d23441a48e516b6c34aea4fa41551a30e30af803  # v6
```

**One deliberate exception:** `release.yml`'s `provenance` job, which calls
`slsa-framework/slsa-github-generator`'s reusable workflow by tag
(`@v2.1.0`), not by SHA. Upstream's own README ("Referencing SLSA builders
and generators") requires its builders and generators be referenced as
`@vX.Y.Z` so that `slsa-verifier` can verify the ref of the trusted
reusable workflow — a hash pin is not supported yet (tracked upstream as
`slsa-verifier#12`). That job's `compile-generator` input also defaults to
false, so its binary-fetch step downloads a release asset at this ref,
which must be a tag. The trust anchor for this one reference is the
Sigstore-signed builder identity, not a SHA pin.

`tests/scripts/test_workflows_are_loadable.py::%test_workflow_uses_are_sha_pinned`
enforces the rule, with the SLSA generator call as its one documented
exemption. `.github/dependabot.yml` bumping these pins, and
`persist-credentials: false` on `actions/checkout` steps, are tracked as
follow-on work in alp-sdk#1544, not yet done.

## Adding a new self-hosted job

Follow the same shape as bitbake: a GitHub-hosted bridge in `alp-sdk`
(guarded, internal-only) that dispatches to `alp-sdk-internal`, whose
self-hosted job runs the work and posts a commit status back. Never
add a `runs-on: [self-hosted, …]` job directly to the public repo.
Hardware-in-the-loop is explicitly **not** a candidate for this
pattern — see [`HW-IN-LOOP.md`](HW-IN-LOOP.md).
