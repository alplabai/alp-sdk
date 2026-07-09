#!/usr/bin/env python3
"""Fail if any version copy drifts from the declared SDK version.

The release flow (`scripts/bump_version.py`, the `cutting-a-release` skill)
updates every place that tracks the version; this check makes any miss a CI
failure instead of a silent drift.  Verified copies:

  1. README.md *current-state* prose labels (MAJOR.MINOR only) -- the README
     sat at "Mostly pre-silicon (`v0.6`)" / "v0.6 ramp" through the entire
     v0.7.0 release before this check existed, plus the "A vX.Y project is
     one declarative file" quick-start line.
  2. include/alp/version.h -- the ALP_VERSION_MAJOR/MINOR/PATCH macros and
     the ALP_VERSION_STRING literal (full MAJOR.MINOR.PATCH).
  3. pyproject.toml -- the alp-sdk-cli `[project]` version (full
     MAJOR.MINOR.PATCH; sat stale at 0.6.0 until v0.8.1).
  4. scripts/alp_cli/__init__.py -- the CLI package `__version__` (full
     triple; this is what `alp --version` actually reports -- it sat at
     "0.6.0" for two releases with nothing catching it, see #445).
  5. src/zephyr/alp_banner.c -- the sample banner line in the file's
     doc-comment (full triple).  The banner *code* always prints the live
     ALP_VERSION_STRING; only the illustrative comment can drift.
  6. The "pin to a release tag -- vX.Y.Z is the latest" west-manifest
     snippet, repeated in README.md and docs/architecture.md (full triple).
  7. docs/board-config.md's directory-tree comment describing
     metadata/sdk_version.yaml's live content (full triple) -- this line
     literally says "currently `version: ...`", i.e. it is NOT a static
     placeholder, it purports to state the real value.

For the README's badge/heading labels this deliberately checks ONLY the
small set of *current-state* labels (anchored by regex), not every `v0.x`
token -- the README's historical references ("the silicon-verified slice
landed in v0.6", "from v0.6 onward") are correct and must stay.  Add a new
anchor here when a new current-state label appears.

Authoritative version: `metadata/sdk_version.yaml` (`version: MAJOR.MINOR.PATCH`).
The MAJOR.MINOR-only anchors track the MAJOR.MINOR (e.g. `v0.7`); the rest
track the full MAJOR.MINOR.PATCH triple.

Exit 0 = in sync, 1 = drift (with the offending lines), 2 = setup error.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys


# (description, compiled regex with ONE capture group = the vMAJOR.MINOR
# token).  Each anchor matches a current-state label whose version MUST
# equal the declared MAJOR.MINOR.  Keep these tightly anchored so historical
# refs ("landed in v0.6") are never matched.
_README_MINOR_ANCHORS: list[tuple[str, re.Pattern[str]]] = [
    ("intro badge — 'Partially silicon-verified (`vX.Y`)'",
     re.compile(r"Partially silicon-verified \(`v(\d+\.\d+)`\)")),
    ("Status heading — '**vX.Y ramp — paper-correct'",
     re.compile(r"\*\*v(\d+\.\d+) ramp — paper-correct")),
    ("Quick-start intro — 'A vX.Y project is one declarative file'",
     re.compile(r"A v(\d+\.\d+) project is \*\*one declarative file\*\*")),
]

# (relpath-under-repo, description, regex) for full MAJOR.MINOR.PATCH
# anchors that live outside version.h/pyproject.toml.  One capture group =
# the vMAJOR.MINOR.PATCH token (without the leading 'v').
_FULL_TRIPLE_DOC_ANCHORS: list[tuple[str, str, re.Pattern[str]]] = [
    ("README.md", "west-manifest pin comment — 'pin to a release tag — vX.Y.Z is the latest'",
     re.compile(r"pin to a release tag — v(\d+\.\d+\.\d+) is the latest")),
    ("docs/architecture.md", "west-manifest pin comment — 'pin to a release tag — vX.Y.Z is the latest'",
     re.compile(r"pin to a release tag — v(\d+\.\d+\.\d+) is the latest")),
    ("docs/board-config.md", "sdk_version.yaml tree comment — 'currently \"version: X.Y.Z\"'",
     re.compile(r'sdk_version\.yaml[^\n]*currently "version: (\d+\.\d+\.\d+)"')),
]


def declared_version(repo: pathlib.Path) -> tuple[int, int, int]:
    """Return the (major, minor, patch) triple from sdk_version.yaml."""
    sdk_version_yaml = repo / "metadata" / "sdk_version.yaml"
    text = sdk_version_yaml.read_text(encoding="utf-8")
    m = re.search(r"^version:\s*(\d+)\.(\d+)\.(\d+)\s*$", text, re.MULTILINE)
    if not m:
        print(f"check_version_doc_sync: could not parse 'version:' from "
              f"{sdk_version_yaml.relative_to(repo)}", file=sys.stderr)
        sys.exit(2)
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def check_readme_minor_anchors(repo: pathlib.Path, want_minor: str) -> list[str]:
    """Check the README current-state prose labels (MAJOR.MINOR only)."""
    readme = repo / "README.md"
    text = readme.read_text(encoding="utf-8")
    drifts: list[str] = []
    for desc, rx in _README_MINOR_ANCHORS:
        found = rx.search(text)
        if found is None:
            drifts.append(f"  MISSING  {desc}: anchor not found in README.md "
                          f"(was it reworded? update scripts/check_version_doc_sync.py)")
            continue
        got = found.group(1)
        if got != want_minor:
            drifts.append(f"  STALE    {desc}: README says v{got}, "
                          f"sdk_version.yaml declares v{want_minor}")
    return drifts


def check_full_triple_doc_anchors(repo: pathlib.Path, want_str: str) -> list[str]:
    """Check the full-triple doc anchors (README/architecture.md/board-config.md)."""
    drifts: list[str] = []
    for rel, desc, rx in _FULL_TRIPLE_DOC_ANCHORS:
        path = repo / rel
        text = path.read_text(encoding="utf-8")
        found = rx.search(text)
        if found is None:
            drifts.append(f"  MISSING  {desc}: anchor not found in {rel} "
                          f"(was it reworded? update scripts/check_version_doc_sync.py)")
            continue
        got = found.group(1)
        if got != want_str:
            drifts.append(f"  STALE    {desc}: {rel} says v{got}, "
                          f"sdk_version.yaml declares v{want_str}")
    return drifts


def check_version_h(repo: pathlib.Path, want: tuple[int, int, int]) -> list[str]:
    """Check include/alp/version.h's ALP_VERSION_* macros (full triple).

    Keep the parsers in lockstep with scripts/bump_version.py's
    update_version_h() rewrite patterns.
    """
    version_h = repo / "include" / "alp" / "version.h"
    rel = version_h.relative_to(repo)
    text = version_h.read_text(encoding="utf-8")
    want_str = ".".join(str(p) for p in want)
    drifts: list[str] = []
    for part, expected in zip(("MAJOR", "MINOR", "PATCH"), want):
        m = re.search(rf"^#define\s+ALP_VERSION_{part}\s+(\d+)", text, re.MULTILINE)
        if m is None:
            drifts.append(f"  MISSING  {rel}: no '#define ALP_VERSION_{part} <n>' macro")
        elif int(m.group(1)) != expected:
            drifts.append(f"  STALE    {rel}: ALP_VERSION_{part} is {m.group(1)}, "
                          f"sdk_version.yaml declares {expected}")
    m = re.search(r'^#define\s+ALP_VERSION_STRING\s+"([^"]*)"', text, re.MULTILINE)
    if m is None:
        drifts.append(f"  MISSING  {rel}: no '#define ALP_VERSION_STRING \"...\"' macro")
    elif m.group(1) != want_str:
        drifts.append(f"  STALE    {rel}: ALP_VERSION_STRING is \"{m.group(1)}\", "
                      f"sdk_version.yaml declares \"{want_str}\"")
    return drifts


def check_pyproject(repo: pathlib.Path, want_str: str) -> list[str]:
    """Check pyproject.toml's [project] version (full triple)."""
    pyproject = repo / "pyproject.toml"
    rel = pyproject.relative_to(repo)
    text = pyproject.read_text(encoding="utf-8")
    m = re.search(r'^version\s*=\s*"([^"]*)"', text, re.MULTILINE)
    if m is None:
        return [f"  MISSING  {rel}: no 'version = \"...\"' line"]
    if m.group(1) != want_str:
        return [f"  STALE    {rel}: version = \"{m.group(1)}\", "
                f"sdk_version.yaml declares \"{want_str}\""]
    return []


def check_cli_init(repo: pathlib.Path, want_str: str) -> list[str]:
    """Check scripts/alp_cli/__init__.py's __version__ (full triple).

    This is what `alp --version` actually reports -- it drifted to a stale
    "0.6.0" for two releases with nothing catching it (#445).
    """
    cli_init = repo / "scripts" / "alp_cli" / "__init__.py"
    rel = cli_init.relative_to(repo)
    text = cli_init.read_text(encoding="utf-8")
    m = re.search(r'^__version__\s*=\s*"([^"]*)"', text, re.MULTILINE)
    if m is None:
        return [f"  MISSING  {rel}: no '__version__ = \"...\"' line"]
    if m.group(1) != want_str:
        return [f"  STALE    {rel}: __version__ = \"{m.group(1)}\", "
                f"sdk_version.yaml declares \"{want_str}\""]
    return []


def check_banner_c(repo: pathlib.Path, want_str: str) -> list[str]:
    """Check src/zephyr/alp_banner.c's sample banner line (full triple).

    The banner *code* prints the live ALP_VERSION_STRING at runtime; only
    the illustrative sample line in the file's doc-comment can drift.
    """
    banner_c = repo / "src" / "zephyr" / "alp_banner.c"
    rel = banner_c.relative_to(repo)
    text = banner_c.read_text(encoding="utf-8")
    m = re.search(r"Alp SDK (\d+\.\d+\.\d+)", text)
    if m is None:
        return [f"  MISSING  {rel}: no 'Alp SDK X.Y.Z' sample banner line"]
    if m.group(1) != want_str:
        return [f"  STALE    {rel}: sample banner says \"Alp SDK {m.group(1)}\", "
                f"sdk_version.yaml declares \"{want_str}\""]
    return []


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1] if __doc__ else "")
    ap.add_argument("--root", default=None,
                     help="Repo root to check (default: this script's repo)")
    args = ap.parse_args()

    repo = pathlib.Path(args.root).resolve() if args.root else pathlib.Path(__file__).resolve().parent.parent

    want = declared_version(repo)
    want_minor = f"{want[0]}.{want[1]}"
    want_str = ".".join(str(p) for p in want)

    drifts = (
        check_readme_minor_anchors(repo, want_minor)
        + check_full_triple_doc_anchors(repo, want_str)
        + check_version_h(repo, want)
        + check_pyproject(repo, want_str)
        + check_cli_init(repo, want_str)
        + check_banner_c(repo, want_str)
    )

    if drifts:
        print(f"Version copies out of sync with metadata/sdk_version.yaml "
              f"(v{want_str}):", file=sys.stderr)
        print("\n".join(drifts), file=sys.stderr)
        print("\nThe release/version bump must update every version copy "
              "(scripts/bump_version.py does; historical 'landed in vX' README "
              "refs stay). -- failing.", file=sys.stderr)
        return 1

    print(f"check_version_doc_sync: OK (README labels, include/alp/version.h, "
          f"pyproject.toml, scripts/alp_cli/__init__.py, src/zephyr/alp_banner.c, "
          f"docs/architecture.md, docs/board-config.md all match v{want_str}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
