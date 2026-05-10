# Contributing to ALP SDK

Canonical contributor guide: [`docs/contribution.md`](docs/contribution.md).
This file is the GitHub-auto-discovered short version.

Thanks for considering a contribution.  The ALP SDK is the unification
software layer for ALP Lab's E1M edge AI modules; keeping it small,
predictable, and OS-pivoted is the whole point.

## How to contribute

### Reporting bugs

1. Check existing [issues](https://github.com/alplabai/alp-sdk/issues).
2. If new, file using the
   [bug report template](https://github.com/alplabai/alp-sdk/issues/new?template=bug_report.md).
3. Include: target SoM, OS backend (`zephyr` / `baremetal` / `yocto`),
   exact `west`/CMake invocation, expected vs. actual behaviour.

### Requesting features

File a [feature request](https://github.com/alplabai/alp-sdk/issues/new?template=feature_request.md)
with the use case and the proposed public-API shape (header
signatures), if you have one in mind.

### Submitting code

1. Fork the repo and branch from `main`: `git checkout -b feature/my-feature`.
2. Keep changes scoped to one library or one SoM at a time.
3. Add or update tests under `tests/`.  Every public function must
   have at least one Unity / ztest test.
4. Run the build matrix locally before opening a PR:
   ```bash
   # Host smoke
   cmake -B build -DALP_BUILD_TESTS=ON
   cmake --build build
   ctest --test-dir build --output-on-failure
   ```
   For Zephyr targets, `west build -b <board>` against your test app.
5. Open a PR; CI runs the AEN-Zephyr, AEN-baremetal, and V2N-Yocto
   matrices.

### Code style

- Public headers are **C99-compatible** with Doxygen comments.
- All public symbols use the `alp_` prefix.
- Keep functions short.  When `src/<os>/<peripheral>.c` exceeds a few
  hundred lines, split by peripheral, not by helper.
- Vendor-specific code lives only in `vendors/<som>/`.  No `#ifdef
  ALIF_*` in `include/alp/` or `src/common/`.

### Adding a new SoM

See [`docs/porting-new-som.md`](docs/porting-new-som.md).

## License

By contributing, you agree your contributions are licensed under
[Apache License 2.0](LICENSE), the SDK's license.

## Getting help

- [Documentation](https://docs.alplab.ai)
- [Community forum](https://community.alplab.ai)
- [GitHub Issues](https://github.com/alplabai/alp-sdk/issues)
