# ci/

CI policy + auxiliary documentation for the ALP SDK.

The actual GitHub Actions workflow files live at
[`/.github/workflows/`](../.github/workflows/) (where GitHub
requires them).  This directory holds the **policy notes,
self-hosted-runner setup, and helper scripts** that the workflows
reference.

## Workflows shipped

| Workflow                                                                       | Trigger          | Status     | What it gates                                                                                  |
|--------------------------------------------------------------------------------|------------------|------------|------------------------------------------------------------------------------------------------|
| [`pr-twister.yml`](../.github/workflows/pr-twister.yml)                        | every PR + push  | active     | west init + west update (cached), twister against `tests/zephyr/**` + `examples/**` on `native_sim/native/64`.  PR fails if any ztest fails. |
| [`pr-metadata-validate.yml`](../.github/workflows/pr-metadata-validate.yml)    | PR + push (paths)| active     | Validates every `metadata/socs/**/*.json` against `metadata/schemas/soc-spec-v1.schema.json` via `scripts/validate_metadata.py`.                |
| [`pr-doxygen.yml`](../.github/workflows/pr-doxygen.yml)                        | PR + push (paths)| active     | Generates Doxygen HTML from `include/alp/**`.  v0.1: warnings are informational; v1.0 will gate on zero warnings (per the quality bar in `VERSIONS.md`). |
| [`nightly-aen-hil.yml`](../.github/workflows/nightly-aen-hil.yml)              | nightly + manual | **skeleton**| HW-in-loop on a real E1M-AEN dev kit.  Runs only when a self-hosted runner with the `hil-aen` label is online — see [`HW-IN-LOOP.md`](HW-IN-LOOP.md).             |

## Workflows planned

| Workflow                                                                       | Target version | Notes                                                                                              |
|--------------------------------------------------------------------------------|----------------|----------------------------------------------------------------------------------------------------|
| `pr-static-analysis.yml`                                                       | v0.2           | clang-tidy + cppcheck on `src/`, `chips/`, and `vendors/`.  Needs a `compile_commands.json` build first. |
| `nightly-v2n-hil.yml`                                                          | v0.2           | HW-in-loop on a real E1M-V2N dev kit.  Runner label `hil-v2n`.                                     |
| `release-abi-snapshot.yml`                                                     | v1.0           | Diffs `include/alp/**` ABI against the previous tag's snapshot; fails on breaking changes after v1.0. |
| `release-publish-doxygen.yml`                                                  | v1.0           | Pushes Doxygen HTML to `gh-pages` on every release tag.                                            |

## Helper scripts

- [`scripts/validate_metadata.py`](../scripts/validate_metadata.py) — runs
  the `pr-metadata-validate` check.  Local invocation:
  ```bash
  pip install jsonschema
  python3 scripts/validate_metadata.py
  ```
- [`scripts/extract_pdf.py`](../scripts/extract_pdf.py) — pypdf
  text extraction used during datasheet ingestion (not in CI; dev tool).
- (planned) `scripts/abi_snapshot.py` — generates a stable ABI
  fingerprint from `include/alp/**` for the v1.0 release gate.

## Runner topology

- **GitHub-hosted**: PR-time workflows (`pr-twister`,
  `pr-metadata-validate`, `pr-doxygen`).  Run on
  `ubuntu-latest`; PR-twister uses the
  `ghcr.io/zephyrproject-rtos/ci` Docker image.
- **Self-hosted (HIL)**: nightly workflows.  See
  [`HW-IN-LOOP.md`](HW-IN-LOOP.md) for runner setup contracts and
  helper-script expectations.

## When to add a new workflow

Match additions to the matrix in [`VERSIONS.md`](../VERSIONS.md):

- A new SoM family in the build matrix → corresponding HIL workflow.
- A new public-API surface → matching twister scenario, plus an
  ABI-snapshot row once we cross v1.0.
- A new metadata schema bump → the `pr-metadata-validate` job
  starts validating against the new schema in addition to v1.

Workflow filenames follow `{stage}-{target}.yml`:

- `stage` is one of `pr` (per-PR), `nightly`, `release`.
- `target` is the SoM family (`aen`, `v2n`, `v2n-m1`) or a global
  scope (`twister`, `doxygen`, `metadata-validate`).
