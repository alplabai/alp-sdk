# CI runner architecture — heavy/self-hosted jobs

`alp-sdk` is a **public** repository. GitHub's own guidance is to never
attach a self-hosted runner to a public repo: a fork pull request would
run untrusted code on your hardware. But the Yocto **bitbake** build and
the Alif **hardware-in-loop (HiL)** flash both need self-hosted runners
(a Yocto host with the licensed RZ BSP; a lab box with a board on USB).

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
| AEN HiL | `self-hosted, linux, hil-aen` | lab box with an Alif dev kit on USB |

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

## Adding a new self-hosted job (e.g. AEN HiL)

Follow the same shape: a GitHub-hosted bridge in `alp-sdk` (guarded,
internal-only) that dispatches to `alp-sdk-internal`, whose self-hosted
job runs the work and posts a commit status back. Never add a
`runs-on: [self-hosted, …]` job directly to the public repo.
