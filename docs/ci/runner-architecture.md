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

## Tokens (fine-grained PATs, or a GitHub App)

| Secret | Lives in | Scope | Used for |
|--------|----------|-------|----------|
| `ALP_INTERNAL_DISPATCH_TOKEN` | **alp-sdk** (public) | `alp-sdk-internal` → Contents: read+write | `repository_dispatch` to the private repo |
| `ALP_SDK_STATUS_TOKEN` | **alp-sdk-internal** (private) | `alp-sdk` → Commit statuses: read+write | post build result back to the PR |

Set them with:

```bash
gh secret set ALP_INTERNAL_DISPATCH_TOKEN  --repo alplabai/alp-sdk           # paste the PAT
gh secret set ALP_SDK_STATUS_TOKEN         --repo alplabai/alp-sdk-internal  # paste the PAT
```

Keep the dispatch token's scope to the single repo + Contents only; it is
the one secret that lives on the public repo (and forks never receive it).

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
