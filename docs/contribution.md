# Contributing to ALP SDK

Canonical contributor documentation for the ALP SDK.  The shorter
[`/CONTRIBUTING.md`](../CONTRIBUTING.md) at the repo root is the
GitHub-auto-discovered entry point and points back here.

## How to contribute

### Reporting bugs

1. Check existing [issues](https://github.com/alplabai/alp-sdk/issues).
2. File via the
   [bug report template](https://github.com/alplabai/alp-sdk/issues/new?template=bug_report.md).
3. Include: target SoM and SKU, OS backend (`zephyr`/`baremetal`/`yocto`),
   exact `west` / CMake invocation, expected vs. actual behaviour,
   relevant logs.

### Requesting features

File via the
[feature request template](https://github.com/alplabai/alp-sdk/issues/new?template=feature_request.md).
Include the use case and the proposed public-API shape (header
signatures), if you have one.

### Submitting code

1. Fork and branch from `main`: `git checkout -b feat/peripherals/whatever`.
2. Keep changes scoped to one library or one SoM at a time.
3. Add or update tests under `tests/`.  Every public function must
   have at least one Unity / ztest test (the v0.1 quality bar).
4. Run the full local matrix before opening a PR:
   ```bash
   bash scripts/bootstrap.sh                    # one-time fresh-clone setup
   export ZEPHYR_BASE="$PWD/../zephyrproject/zephyr"
   bash scripts/test-all.sh                     # ctest + twister + format + Doxygen
   ```
   `test-all.sh` accepts `--quick` (skip twister + Doxygen),
   `--yocto-only`, `--zephyr-only`, and `--no-clean` for tighter
   inner loops.  See [`docs/testing.md`](testing.md) for the
   per-stage breakdown.
5. Open a PR.  CI runs the AEN-Zephyr, AEN-baremetal, and V2N-Yocto
   matrices (whichever exist for the version you're branching from —
   see [`VERSIONS.md`](../VERSIONS.md)).  Append a row to
   [`docs/test-plan.md`](test-plan.md) for any new feature, default
   `⏳ untested`; CI green is necessary but not sufficient to flip
   the row to `✅` -- real-hardware HIL evidence does.

## Code style

### Public surface (`include/alp/`)

- C99-compatible.  Doxygen comments on every public function, type,
  and macro.
- All public symbols use the **`alp_` prefix**: `alp_i2c_t`,
  `alp_spi_open()`, `alp_display_blit()`, `alp_iot_*`, …
- Lowercase, snake_case.
- Opaque types (forward-declared structs returned as pointers)
  unless there's a strong reason to expose layout.

### Chip drivers (`chips/<part>/`, `include/alp/chips/<part>.h`)

- Symbols use the **chip's natural name** as the prefix:
  `lsm6dso_t`, `lsm6dso_init()`, `ssd1306_clear()`, `ov5640_capture()`.
  No `alp_` prefix.  The `alp_` namespace is reserved for SDK
  abstractions; chip drivers are bindings to third-party silicon and
  do not carry it.
- Chip drivers consume the SDK's `<alp/peripheral.h>` etc. — never
  the underlying Zephyr / vendor HAL directly.  This keeps every
  chip driver portable across the three OS targets.

### Vendor wrappers (`vendors/<som-slug>/`)

- Internal-only.  Not exposed to applications.
- Naming: `vendors_<slug>_<peripheral>_<verb>()`.
- Called from `src/<os>/peripheral.c` only.

### General rules

- Keep functions short.  When `src/<os>/<peripheral>.c` exceeds a few
  hundred lines, split by peripheral, not by helper.
- No `#ifdef ALIF_*` or `#ifdef RENESAS_*` in `include/alp/` or
  `src/common/`.  Vendor-specific code lives only in `vendors/`.
- No GPL dependencies.  Apache-2.0 / MIT / BSD only.

## Adding a new SoM

See [`docs/porting-new-som.md`](porting-new-som.md).  Summary:

1. Add a column to [`docs/os-support-matrix.md`](os-support-matrix.md).
2. Add `vendors/<som-slug>/`.
3. Hook it into the relevant OS backend (`src/zephyr/`,
   `src/baremetal/`, or `src/yocto/`).
4. Add a metadata file at
   `metadata/socs/<vendor>/<family>/<part>.json` validating against
   the v1 schema.
5. Update CI matrices.

## Adding a new chip driver

1. Create `chips/<part>/` with `<part>.c`, optional OS-specific glue,
   and tests.
2. Create `include/alp/chips/<part>.h` (no `alp_` symbol prefix).
3. Add unit tests under `chips/<part>/tests/`.
4. Add a row to the v0.x section of [`VERSIONS.md`](../VERSIONS.md)
   listing the chip and the alp-studio block that consumes it.

## ABI policy

- Pre-1.0: API may change between minors during development.
- v1.0+: ABI is frozen.  Breaking changes require a major bump.
- A function may be deprecated for at least 2 minor versions before
  removal (`__attribute__((deprecated("message")))`).  Removal still
  requires a major bump.
- Each release tag carries an ABI snapshot diffed against the
  previous; CI fails on breaking diffs after v1.0.

## License

By contributing, you agree your contributions are licensed under
[Apache License 2.0](../LICENSE), the SDK's license.

## Getting help

- [Documentation](https://docs.alplab.ai)
- [Community forum](https://community.alplab.ai)
- [GitHub Issues](https://github.com/alplabai/alp-sdk/issues)
