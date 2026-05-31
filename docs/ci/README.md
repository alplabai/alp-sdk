@page docs_ci_index CI policy

# docs/ci/

CI policy + auxiliary documentation for the Alp SDK.

The actual GitHub Actions workflow files live at
[`/.github/workflows/`](../../.github/workflows/) (where GitHub
requires them).  This directory holds the **policy notes,
self-hosted-runner setup, and helper scripts** that the workflows
reference.

## Workflows shipped

| Workflow                                                                       | Trigger          | Status     | What it gates                                                                                  |
|--------------------------------------------------------------------------------|------------------|------------|------------------------------------------------------------------------------------------------|
| [`pr-twister.yml`](../../.github/workflows/pr-twister.yml)                        | every PR + push  | active     | Runs on `ubuntu-latest` (no docker container) with `ZEPHYR_TOOLCHAIN_VARIANT=host` so native_sim uses the runner's stock gcc.  west init + west update (cached), twister against `tests/zephyr/**` + `examples/**` on `native_sim/native/64`.  PR fails if any ztest fails. |
| [`pr-plain-cmake.yml`](../../.github/workflows/pr-plain-cmake.yml)                | PR + push (paths)| active     | Plain-CMake builds for `ALP_OS=baremetal`, `ALP_OS=baremetal -DALP_SOM={aen,v2n}`, and `ALP_OS=yocto` with `ALP_BUILD_TESTS=ON`.  Installs `libmosquitto-dev` + `libasound2-dev` + `libssl-dev` + `pkg-config` so the Yocto-side wrappers (MQTT, ALSA audio, OpenSSL security) compile + their ctest binaries run. |
| [`pr-static-analysis.yml`](../../.github/workflows/pr-static-analysis.yml)        | PR + push        | active     | `clang-format-diff` on changed lines + `cppcheck` informational pass over `src/` + `chips/`.  Diff-only format check; v0.2 will gate on full tree. |
| [`pr-generated-files.yml`](../../.github/workflows/pr-generated-files.yml)        | PR + push (paths)| active     | Catches drift in `<alp/soc_caps.h>` (re-runs `scripts/gen_soc_caps.py`) and `docs/abi/*.json` (re-runs `scripts/abi_snapshot.py`).             |
| [`pr-metadata-validate.yml`](../../.github/workflows/pr-metadata-validate.yml)    | PR + push (paths)| active     | Validates every `metadata/socs/**/*.json` against the schema via `scripts/validate_metadata.py` + smoke-tests `scripts/alp_project.py` against `metadata/templates/board.yaml.example`. |
| [`pr-doxygen.yml`](../../.github/workflows/pr-doxygen.yml)                        | PR + push (paths)| active     | Generates Doxygen HTML from `include/alp/**`.  Runs with `FAIL_ON_WARNINGS=YES` — zero warnings required; PR fails on any warning. |
| [`coverity.yml`](../../.github/workflows/coverity.yml)                            | weekly + manual  | active     | Coverity Scan submission against <https://scan.coverity.com/projects/alplabai-alp-sdk>.  Secrets (`COVERITY_TOKEN`, `COVERITY_EMAIL`) provisioned; project name in the `COVERITY_PROJECT` Actions variable.       |
| [`nightly-aen-hil.yml`](../../.github/workflows/nightly-aen-hil.yml)              | nightly + manual | **skeleton**| HW-in-loop on a real E1M-AEN dev kit.  Runs only when a self-hosted runner with the `hil-aen` label is online — see [`HW-IN-LOOP.md`](HW-IN-LOOP.md).             |

## Workflows planned

| Workflow                                                                       | Target version | Notes                                                                                              |
|--------------------------------------------------------------------------------|----------------|----------------------------------------------------------------------------------------------------|
| `nightly-yocto-hil.yml`                                                        | v0.4           | HW-in-loop on a real V2N or i.MX 93 EVK running a meta-alp-sdk Yocto image.  Runner label `hil-yocto`.  Flips every 🟡 Yocto row in `docs/test-plan.md` to ✅ on a green run. |
| `nightly-v2n-m1-hil.yml`                                                       | v0.4           | HW-in-loop on a real V2N + DEEPX DX-M1 dev kit.  Runner label `hil-v2n-m1`.                        |
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
  diffs against `docs/abi/v0.1-snapshot.json` after v1.0.
- [`scripts/bootstrap.sh`](../../scripts/bootstrap.sh) — fresh-clone
  developer setup (west workspace + Python deps + apt hints).
  Not in CI; the CI workflows install equivalents inline.
- [`scripts/test-all.sh`](../../scripts/test-all.sh) — single-command
  local verifier (ctest + twister + clang-format + metadata-validate +
  Doxygen).  Mirrors every PR-time workflow except `coverity`.  See
  [`docs/testing.md`](../testing.md).  (The VS Code extension's
  build lives in [`alplabai/alp-sdk-vscode`](https://github.com/alplabai/alp-sdk-vscode)
  since the 2026-05-12 split; its own CI runs there.)

## Runner topology

- **GitHub-hosted**: PR-time workflows (`pr-twister`,
  `pr-metadata-validate`, `pr-doxygen`).  Run on
  `ubuntu-latest`; PR-twister uses the
  `ghcr.io/zephyrproject-rtos/ci` Docker image.
- **Self-hosted (HIL)**: nightly workflows.  See
  [`HW-IN-LOOP.md`](HW-IN-LOOP.md) for runner setup contracts and
  helper-script expectations.

## When to add a new workflow

Match additions to the matrix in [`VERSIONS.md`](../../VERSIONS.md):

- A new SoM family in the build matrix → corresponding HIL workflow.
- A new public-API surface → matching twister scenario, plus an
  ABI-snapshot row once we cross v1.0.
- A new metadata schema bump → the `pr-metadata-validate` job
  starts validating against the new schema in addition to v1.

Workflow filenames follow `{stage}-{target}.yml`:

- `stage` is one of `pr` (per-PR), `nightly`, `release`.
- `target` is the SoM family (`aen`, `v2n`, `v2n-m1`) or a global
  scope (`twister`, `doxygen`, `metadata-validate`).
