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
4. **Append a row to [`docs/test-plan.md`](docs/test-plan.md).**
   New feature lands as `⏳ untested` by default; if the PR also
   captures verification evidence (HIL log, broker roundtrip,
   scope capture, …), flip the row to `🟡 partial` or `✅
   verified` and link the evidence in the Notes column.  Reserve
   `❌` for **failing** verification (the contract was tested
   and didn't hold) -- that's a blocker, not a default state.
   A feature that doesn't appear in the test plan does **not**
   ship -- code that nobody has committed to verifying isn't a
   release deliverable.
5. Run the full local matrix before opening a PR:
   ```bash
   bash scripts/bootstrap.sh                    # one-time
   export ZEPHYR_BASE="$PWD/../zephyrproject/zephyr"
   bash scripts/test-all.sh                     # ctest + twister + format + Doxygen
   ```
   See [`docs/testing.md`](docs/testing.md) for the per-stage
   breakdown + how to run individual layers.
6. Open a PR; CI runs the AEN-Zephyr, AEN-baremetal, and V2N-Yocto
   matrices.  CI green is necessary but not sufficient for tagging
   a release -- the test-plan row also has to flip to `✅`.

### Code style

- Public headers are **C99-compatible** with Doxygen comments.
- All public symbols use the `alp_` prefix.
- Keep functions short.  When `src/<os>/<peripheral>.c` exceeds a few
  hundred lines, split by peripheral, not by helper.
- Vendor-specific code lives only in `vendors/<som>/`.  No `#ifdef
  ALIF_*` in `include/alp/` or `src/common/`.

### Formatting

The tree is formatted with **clang-format v14** (the Ubuntu 22.04 apt
default that `pr-static-analysis` pins via `update-alternatives`).
Mismatched versions reflow braces, trailing-comment columns, and
`AlignConsecutive*` columns differently, which silently breaks the
diff-only CI gate even when no source actually changed style.

Pin your local installation before pushing:

```bash
bash scripts/setup-clang-format.sh           # install + verify (apt / brew)
bash scripts/setup-clang-format.sh --check   # verify only (no install)
```

`.clang-format` at the repo root carries the style spec (LLVM base +
`Cpp11BracedListStyle: false` to keep designated-initialiser brace
spacing stable across v14 / v18+).  See
[`docs/contribution.md`](docs/contribution.md#formatting) for platform
notes and the manual-install fallback for non-Debian Linux / Windows.

### Adding a new SoM

See [`docs/porting-new-som.md`](docs/porting-new-som.md).

## License

By contributing, you agree your contributions are licensed under
[Apache License 2.0](LICENSE), the SDK's license.

## Credit

We use the standard Git `Co-Authored-By:` trailer convention.  Every
commit that takes meaningful work from someone other than the
committer ends with:

```
Co-Authored-By: Name <email@example.com>
```

GitHub picks the trailer up and adds the named address to the commit's
co-author list + the repository's Contributors graph.  See
[GitHub Docs · Creating a commit with multiple authors](https://docs.github.com/en/pull-requests/committing-changes-to-your-project/creating-and-editing-commits/creating-a-commit-with-multiple-authors)
for the formatting rules.

## Team

The ALP SDK is built and maintained by [Alp Lab](https://alplab.ai).

| Contributor          | GitHub                                                   | Role                          |
|----------------------|----------------------------------------------------------|-------------------------------|
| Caner Alp            | [@alpCaner](https://github.com/alpCaner)                 | Founder · SDK architect       |
| Hakan Gülen          | [@hkngln](https://github.com/hkngln)                     | Maintainer                    |
| Şükrü Sinan Aydoğdu  | [@sukru-aydogdu](https://github.com/sukru-aydogdu)       | Maintainer                    |
| globglob             | [@globglob3D](https://github.com/globglob3D)             | Contributor                   |
| Sri (Alp Lab)        | [@Sri-AlpLab](https://github.com/Sri-AlpLab)             | Contributor                   |

Additional Alp Lab team members extend this table from their own
commits as their work lands.

## Getting help

- [Documentation](https://docs.alplab.ai)
- [Community forum](https://community.alplab.ai)
- [GitHub Issues](https://github.com/alplabai/alp-sdk/issues)
