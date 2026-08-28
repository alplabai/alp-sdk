@page docs_ci_index CI policy

# docs/ci/

CI policy + auxiliary documentation for the Alp SDK.

The actual GitHub Actions workflow files live at
[`/.github/workflows/`](../../.github/workflows/) (where GitHub
requires them).  This directory holds the **policy notes,
self-hosted-runner setup, and helper scripts** that the workflows
reference.

## Workflows shipped

`.github/workflows/` carries **23** workflow files as of this revision
(counted via `ls .github/workflows/*.yml .github/workflows/*.yaml
2>/dev/null | wc -l`; recount before trusting this number, it moves
every time a workflow is added or retired).  The table below is a
**curated subset** — the gates most PRs interact with directly — not
an exhaustive list; the directory linked above is authoritative.  Two
rows that used to live here were
dropped because the workflow no longer exists: `issue-auto-label.yml`
(deleted; see the `chore(ci): remove issue-auto-label workflow`
commit) and `nightly-aen-hil.yml` (retired — CI does not drive the AEN
bench; see [`HW-IN-LOOP.md`](HW-IN-LOOP.md) for the bench-run contract
that replaced it).

| Workflow                                                                       | Trigger          | Status     | What it gates                                                                                  |
|--------------------------------------------------------------------------------|------------------|------------|------------------------------------------------------------------------------------------------|
| [`pr-twister.yml`](../../.github/workflows/pr-twister.yml)                        | every PR + push  | active     | Runs on `ubuntu-latest` (no docker container) with `ZEPHYR_TOOLCHAIN_VARIANT=host` so native_sim uses the runner's stock gcc.  west init + west update (cached), twister against `tests/zephyr/**` + `examples/**` on `native_sim/native/64`.  PR fails if any ztest fails. |
| [`pr-plain-cmake.yml`](../../.github/workflows/pr-plain-cmake.yml)                | PR + push (paths)| active     | Plain-CMake builds for `ALP_OS=baremetal`, `ALP_OS=baremetal -DALP_SOM={aen,v2n}`, and `ALP_OS=yocto` with `ALP_BUILD_TESTS=ON`.  Installs `libmosquitto-dev` + `libasound2-dev` + `libssl-dev` + `pkg-config` so the Yocto-side wrappers (MQTT, ALSA audio, OpenSSL security) compile + their ctest binaries run. |
| [`pr-static-analysis.yml`](../../.github/workflows/pr-static-analysis.yml)        | PR + push        | active     | `clang-format-diff` on changed lines + `shellcheck` over every shipped `*.sh` (repo-wide `git ls-files` sweep over `*.sh`, issue #1550; `-x -S warning` for `scripts/bench/**` and `scripts/test-all.sh`, `-S error` elsewhere), both in the `clang-format-diff` job so a shellcheck defect hard-blocks too (`clang-format · diff-only` is one of `dev`'s required contexts; a separate job's context is not) + `cppcheck` informational pass over `src/` + `chips/` in its own non-required job. |
| [`pr-generated-files.yml`](../../.github/workflows/pr-generated-files.yml)        | PR + push (paths)| active     | Catches drift in `<alp/soc_caps.h>` (re-runs `scripts/gen_soc_caps.py`) and `docs/abi/*.json` (re-runs `scripts/abi_snapshot.py`).             |
| [`pr-metadata-validate.yml`](../../.github/workflows/pr-metadata-validate.yml)    | PR + push (paths)| active     | Validates every `metadata/socs/**/*.json` against the schema via `scripts/validate_metadata.py` + smoke-tests `scripts/alp_project.py` against `metadata/templates/board.yaml.example`. |
| [`pr-doxygen.yml`](../../.github/workflows/pr-doxygen.yml)                        | PR + push (paths)| active     | Generates Doxygen HTML from `include/alp/**`.  Runs with `FAIL_ON_WARNINGS=YES` — zero warnings required; PR fails on any warning. |
| [`coverity.yml`](../../.github/workflows/coverity.yml)                            | weekly + manual  | active     | Coverity Scan submission against <https://scan.coverity.com/projects/alplabai-alp-sdk>.  Secrets (`COVERITY_TOKEN`, `COVERITY_EMAIL`) provisioned; project name in the `COVERITY_PROJECT` Actions variable.       |
| [`pr-bitbake.yml`](../../.github/workflows/pr-bitbake.yml)                        | PR to `main` (paths) | active | Dispatch bridge to the private `alp-sdk-internal` repo's self-hosted Yocto runner — see [`runner-architecture.md`](runner-architecture.md). |
| [`onramp-clean-container.yml`](../../.github/workflows/onramp-clean-container.yml)| PR (paths) + weekly + manual + `run-full-quickstart` label | active | Runs the documented first-install journey (`docs/getting-started.md` §1–4) inside a genuinely bare `ubuntu:24.04` container — no apt package this job doesn't itself install. `prereqs-and-bootstrap` (every relevant PR) proves `bash scripts/bootstrap.sh` refuses with actionable hints then succeeds. `full-quickstart-build` (weekly cron + `workflow_dispatch` — both inert until this file reaches the default branch — or a PR carrying the `run-full-quickstart` label) walks the rest: installs `tan`, `west sdk install`s the Zephyr SDK, `tan build --sdk-root` (plus a `tan init` scaffold and a build of it), and asserts a real `zephyr.elf` came out. See issue #949. |
| [`pr-bootstrap-distro-install.yml`](../../.github/workflows/pr-bootstrap-distro-install.yml)| PR (paths) | active | Container-job proof for `metadata/bootstrap.json`'s `prerequisites.install.linux` (issue #1464): a 3-leg matrix (`debian:12`/apt, `fedora:42`/dnf, `rockylinux:9`/dnf) derives the install commands from the manifest at run time, actually runs them, and asserts every declared tool lands on `PATH` — the admission bar that keeps a guessed package name from ever shipping. |

## Workflows planned

On-silicon verification (AEN, V2N/V2N-M1, ...) is deliberately **not**
in this table any more: CI does not drive the bench.  It is an
explicitly-invoked run under a held labgrid reservation, with its
result attached to the PR or release that needs it — see
[`HW-IN-LOOP.md`](HW-IN-LOOP.md).

| Workflow                                                                       | Target version | Notes                                                                                              |
|--------------------------------------------------------------------------------|----------------|----------------------------------------------------------------------------------------------------|
| `release-abi-snapshot.yml`                                                     | v1.0           | Diffs `include/alp/**` ABI against the previous tag's snapshot; fails on breaking changes after v1.0. |
| `release-publish-doxygen.yml`                                                  | v1.0           | Pushes Doxygen HTML to `gh-pages` on every release tag.                                            |

## Helper scripts

- [`scripts/validate_metadata.py`](../../scripts/validate_metadata.py) — runs
  the `pr-metadata-validate` check.  Local invocation:
  ```bash
  pip install jsonschema
  python3 scripts/validate_metadata.py
  ```
- [`scripts/extract_pdf.py`](../../scripts/extract_pdf.py) — pypdf
  text extraction used during datasheet ingestion (not in CI; dev tool).
- [`scripts/abi_snapshot.py`](../../scripts/abi_snapshot.py) — generates a
  stable ABI fingerprint from `include/alp/**`.  Re-run by
  `pr-generated-files.yml` to catch drift; gates `include/alp/**`
  diffs against `docs/abi/v<MINOR>-snapshot.json` — the snapshot for the
  version `metadata/sdk_version.yaml` declares (`v0.16-snapshot.json`
  today) — after v1.0.
- [`scripts/bootstrap.sh`](../../scripts/bootstrap.sh) — fresh-clone
  developer setup (west workspace + Python deps + apt hints).
  Not in CI; the CI workflows install equivalents inline.
- [`scripts/test-all.sh`](../../scripts/test-all.sh) — single-command
  local verifier (ctest + twister + clang-format + metadata-validate +
  Doxygen + the required `scripts/check_*.py` gate registry, see
  `--list-required-gate-scripts`).  It covers the GitHub-hosted,
  hardware-free PR gates; it does **not** cover the self-hosted/bitbake
  build (`pr-bitbake.yml`), the GD32 / CC3501E bridge-firmware builds,
  or the AEN onramp quickstart container — those still need a push
  through CI (or their own local invocation) to exercise.  See
  [`docs/testing.md`](../testing.md).  (The VS Code extension's
  build lives in [`alplabai/alp-sdk-vscode`](https://github.com/alplabai/alp-sdk-vscode)
  since the 2026-05-12 split; its own CI runs there.)

## Runner topology

- **GitHub-hosted**: PR-time workflows (`pr-twister`,
  `pr-metadata-validate`, `pr-doxygen`).  Run on
  `ubuntu-latest`; PR-twister deliberately uses the runner's
  stock gcc (`ZEPHYR_TOOLCHAIN_VARIANT=host`), not the
  `ghcr.io/zephyrproject-rtos/ci` Docker image.
- **Self-hosted (heavy builds)**: the bitbake (Yocto) job never attaches
  a runner to this **public** repo.  It runs on a self-hosted runner
  registered to the **private** `alp-sdk-internal` repo, triggered by a
  dispatch bridge (`pr-bitbake.yml`); results post back as commit
  statuses on the PR.  See [`runner-architecture.md`](runner-architecture.md)
  for the full model.
- **No HIL runner, public or private.**  On-silicon verification is
  not CI at all: SETOOLS is license-gated and must not be redistributed
  to a shared runner, and the bench is a strictly serial,
  labgrid-reservation-gated resource — no runner-based flow is lawful
  or workable here.  It is an explicitly-invoked bench run instead; see
  [`HW-IN-LOOP.md`](HW-IN-LOOP.md) for that contract.

## When to add a new workflow

Match additions to the matrix in [`VERSIONS.md`](../../VERSIONS.md):

- A new SoM family in the build matrix → a corresponding bench-run
  entry (`tests/hil/<sku>-<board>/`), not a CI workflow — see
  [`HW-IN-LOOP.md`](HW-IN-LOOP.md).
- A new public-API surface → matching twister scenario, plus an
  ABI-snapshot row once we cross v1.0.
- A new metadata schema bump → the `pr-metadata-validate` job
  starts validating against the new schema in addition to v1.

Workflow filenames follow `{stage}-{target}.yml`:

- `stage` is one of `pr` (per-PR), `nightly`, `release`.
- `target` is the SoM family (`aen`, `v2n`, `v2n-m1`) or a global
  scope (`twister`, `doxygen`, `metadata-validate`).

Every job needs a `timeout-minutes:` (#1477 -- GitHub's implicit
360-minute runner default otherwise applies silently). Every job's ceiling
must stay strictly above the sum of its own step-level `timeout-minutes:`
plus 1 minute for each step that has none, or a step running late can
still be killed by the job-level timeout before its own fires --
`tests/scripts/test_tier_a_workflow_step_timeouts.py` enforces both, over
every file under `.github/workflows/`. Derive each ceiling from real
observed run durations (`gh api .../actions/runs/<id>/jobs`), not a guess;
comment the derivation at the site for anything non-obvious.
