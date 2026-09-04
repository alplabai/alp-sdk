@page docs_doxygen_index Doxygen reference build

# docs/doxygen/ — generated API reference

Doxygen configuration for the SDK's public-API reference lives here.
The generator runs on every relevant PR via
[`.github/workflows/pr-doxygen.yml`](../../.github/workflows/pr-doxygen.yml)
and is a hard CI gate: any Doxygen warning fails the build.

- [`Doxyfile`](Doxyfile) — the single source of the config (issue
  #970), consumed by both `pr-doxygen.yml` and `scripts/test-all.sh`'s
  local `doxygen` stage so the two can never drift into two different
  configs.  `INPUT` covers `include/alp/**` (the public surface --
  `src/`, `chips/<part>/`, and `vendors/` headers are intentionally
  excluded) plus `README.md`, `VERSIONS.md`, `CONTRIBUTING.md`,
  `TRADEMARKS.md`, `docs/`, and the per-area landing `README.md` files
  (`chips/README.md`, `vendors/alif/README.md`,
  `vendors/deepx-dxm1/README.md`,
  `vendors/gd32_firmware_library/README.md`,
  `cc3501e-bridge-firmware:README.md`, `keys/README.md`,
  `meta-alp-sdk/README.md`, `examples/README.md`,
  `metadata/library-profiles/README.md`,
  `zephyr/sysbuild/aen/README.md`).  `USE_MDFILE_AS_MAINPAGE =
  README.md` makes the repo root README the generated site's landing
  page -- there is no separate `docs/doxygen/pages/` directory; each
  area's own `README.md` fills that role in place.  `docs/superpowers/*`
  (internal working notes) is excluded via `EXCLUDE_PATTERNS`.
- [`scripts/check_doxygen_coverage.py --fail-on-gaps`](../../scripts/check_doxygen_coverage.py)
  runs first, as a fast pre-flight `@brief` coverage audit, before the
  full HTML build.
- `PROJECT_NUMBER` is the one value the committed `Doxyfile` does
  *not* carry (it varies per commit); both the CI workflow and
  `scripts/test-all.sh` append it on stdin from `git describe --tags`.
