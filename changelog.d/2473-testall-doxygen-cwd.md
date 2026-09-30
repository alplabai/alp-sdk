### Fixed — `test-all.sh` doxygen stage pinned its own working directory (#2473)

`stage_doxygen` piped `docs/doxygen/Doxyfile` into `doxygen` without ever
forcing the directory doxygen actually inherits. In a shared gw queue slot the
shell's current directory had drifted by the time this stage ran, so doxygen
resolved the relative markdown links in `docs/**/*.md` against the wrong base
and reported `unable to resolve reference to
'<home>/vendors/alif/README.md'` for the link at
`docs/boards/e1m-evk.md:419` ("](../../vendors/alif/README.md)") —
a false FAIL, since a fresh `--depth 1` clone of the identical commit built
with 0 warnings.

The doxygen invocation now runs in its own subshell that `cd`s to
`${REPO_ROOT}` first, at `scripts/test-all.sh:1294` ("cat docs/doxygen/Doxyfile"),
so the stage's result no longer depends on whatever left the shell's cwd
wherever it was before this stage ran.

`tests/scripts/test_test_all_doxygen_cwd.py` drives the real `stage_doxygen`
from a decoy directory with a fake `doxygen` that records where it was
started, and fails against the pre-fix stage.
