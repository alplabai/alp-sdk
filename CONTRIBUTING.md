@page contributing_index Contributing

# Contributing to Alp SDK

Canonical contributor guide: [`docs/contribution.md`](docs/contribution.md).
This file is the GitHub-auto-discovered short version.

Thanks for considering a contribution.  The Alp SDK is the unification
software layer for Alp Lab's E1M edge AI modules; keeping it small,
predictable, and OS-pivoted is the whole point.

## Where to ask things

| Channel | Use it for |
|---|---|
| [`community.alplab.ai`](https://community.alplab.ai/) | Open-ended questions, "how do I...", design discussions, showcasing what you built |
| [GitHub Issues](https://github.com/alplabai/alp-sdk/issues) | Concrete bugs, feature requests, regressions, anything with a reproducer |
| [GitHub Security Advisories](https://github.com/alplabai/alp-sdk/security/advisories) | **Security issues only** — see [`docs/security-advisories.md`](docs/security-advisories.md) for the report flow |
| [`docs.alplab.ai/sdk/introduction`](https://docs.alplab.ai/sdk/introduction) | Rendered docs (mirror of the in-repo markdown with cross-version nav + search) |

If you're unsure whether it's a bug or a usage question, start on
the community forum — bugs that need the issue tracker get triaged
across.

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

1. Fork the repo and branch from `dev`: `git checkout -b feat/my-feature`.
   Feature branches branch off `dev` and merge back into `dev` via PR
   (`--no-ff`); use a type prefix (`feat/`, `fix/`, `docs/`, ...).
   `dev` is the shared integration branch; `main` is the tested,
   releasable baseline `dev` is promoted to at the release gate.
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

   Don't want to set up west + Zephyr by hand?
   [`tools/native-sim-container/`](tools/native-sim-container/)
   freezes the `native_sim` PR gate in a container --
   `make -C tools/native-sim-container test` reproduces
   `pr-twister.yml` locally with no hardware.

   **Python version:** the support *floor* is 3.10
   (`pyproject.toml` `requires-python`), but dev/CI standardise on
   the *pin* in the repo-root `.python-version` (currently 3.12) --
   every CI workflow's `actions/setup-python` reads that one file.
   Run the pinned version locally to match CI exactly (`pyenv`/`uv`
   pick it up automatically); `tan doctor` warns on a mismatch.
6. Open a PR; CI runs the AEN-Zephyr, AEN-baremetal, and V2N-Yocto
   matrices.  CI green is necessary but not sufficient for tagging
   a release -- the test-plan row also has to flip to `✅`.

For the full branching topology, merge methods, PR gates,
backport flow, and branch-protection settings, see
[`docs/branching-and-merge-policy.md`](docs/branching-and-merge-policy.md).

### Sign your work (Developer Certificate of Origin)

Contributions are accepted under the project's Apache-2.0 license (see
[`LICENSE`](LICENSE) / [`NOTICE`](NOTICE)).  To certify that you wrote the
patch — or otherwise have the right to submit it under that license — **sign
off every commit** per the
[Developer Certificate of Origin 1.1](https://developercertificate.org):

```bash
git commit -s -m "your message"
```

This appends a `Signed-off-by: Your Name <you@example.com>` line from your
`git config user.name` / `user.email`.  Commits without a sign-off may be
asked to amend.  No separate CLA is required.  (Trademark use is governed
separately — see [`TRADEMARKS.md`](TRADEMARKS.md).)

### Code style

- Public headers are **C99-compatible** with Doxygen comments.
- All public symbols use the `alp_` prefix.
- Keep functions short.  When `src/<os>/<peripheral>.c` exceeds a few
  hundred lines, split by peripheral, not by helper.
- Vendor-specific code lives only in `vendors/<som>/`.  No `#ifdef
  ALIF_*` in `include/alp/` or `src/common/`.

### Formatting

Alp SDK is its **own** code style — a unification layer across hardware
and software platforms, not a Zephyr project — so it has a house style.
Native C is **TAB-indented** (`.clang-format`: `UseTab: ForIndentation`);
render tabs at whatever width reads best for you, since the file stores
tabs rather than a fixed number of spaces.  Alignment stays in spaces, so
columns line up at any tab width.  A `.editorconfig` sets this up
automatically in most editors.

The tree is formatted with **clang-format v22**, pinned via the
`clang-format` pip wheel (`clang-format==22.1.5`) that
`pr-static-analysis` installs.  We pin via the wheel, not apt: apt's
`clang-format` floats with the distro and v22 is not packaged at all.
Mismatched versions reflow braces, trailing-comment columns, and
`AlignConsecutive*` columns differently, which silently breaks the
diff-only CI gate even when no source actually changed style.

Pin your local installation before pushing:

```bash
bash scripts/setup-clang-format.sh           # install + verify (pip wheel)
bash scripts/setup-clang-format.sh --check   # verify only (no install)
```

Optionally, enable the local pre-commit hook so formatting is checked
before every commit (mirrors the CI gate, opt-in):

```bash
pip install pre-commit && pre-commit install
```

`.clang-format` at the repo root carries the style spec (LLVM base, tabs,
`Cpp11BracedListStyle: false` to keep designated-initialiser brace
spacing stable across v14 / v18+).  Vendored upstream trees (`zephyr/**`,
`vendors/**`) keep their own style and are excluded from the gate.  See
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

The Alp SDK is built and maintained by [Alp Lab](https://alplab.ai).

| Contributor          | GitHub                                                   | Role                          |
|----------------------|----------------------------------------------------------|-------------------------------|
| Caner Alp            | [alpCaner](https://github.com/alpCaner)                  | Founder · SDK architect       |
| Hakan Gülen          | [hkngln](https://github.com/hkngln)                      | Maintainer                    |
| Şükrü Sinan Aydoğdu  | [sukru-aydogdu](https://github.com/sukru-aydogdu)        | Maintainer                    |
| globglob             | [globglob3D](https://github.com/globglob3D)              | Contributor                   |
| Sri (Alp Lab)        | [Sri-AlpLab](https://github.com/Sri-AlpLab)              | Contributor                   |

Additional Alp Lab team members extend this table from their own
commits as their work lands.

## Getting help

- [Documentation](https://docs.alplab.ai)
- [Community forum](https://community.alplab.ai)
- [GitHub Issues](https://github.com/alplabai/alp-sdk/issues)
