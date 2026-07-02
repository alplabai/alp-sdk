#!/usr/bin/env python3
"""Fail if any version copy drifts from the declared SDK version.

The release flow (`scripts/bump_version.py`, the `cutting-a-release` skill)
updates every place that tracks the version; this check makes any miss a CI
failure instead of a silent drift.  Verified copies:

  1. README.md *current-state* prose labels (MAJOR.MINOR only) -- the README
     sat at "Mostly pre-silicon (`v0.6`)" / "v0.6 ramp" through the entire
     v0.7.0 release before this check existed.
  2. include/alp/version.h -- the ALP_VERSION_MAJOR/MINOR/PATCH macros and
     the ALP_VERSION_STRING literal (full MAJOR.MINOR.PATCH).
  3. pyproject.toml -- the alp-sdk-cli `[project]` version (full
     MAJOR.MINOR.PATCH; sat stale at 0.6.0 until v0.8.1).

For the README it deliberately checks ONLY the small set of *current-state*
labels (anchored by regex), not every `v0.x` token -- the README's historical
references ("the silicon-verified slice landed in v0.6", "from v0.6 onward")
are correct and must stay.  Add a new anchor here when a new current-state
label appears.

Authoritative version: `metadata/sdk_version.yaml` (`version: MAJOR.MINOR.PATCH`).
The README labels track the MAJOR.MINOR (e.g. `v0.7`); the patch is not in prose.

Exit 0 = in sync, 1 = drift (with the offending lines), 2 = setup error.
"""
from __future__ import annotations

import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
SDK_VERSION_YAML = REPO / "metadata" / "sdk_version.yaml"
README = REPO / "README.md"
VERSION_H = REPO / "include" / "alp" / "version.h"
PYPROJECT = REPO / "pyproject.toml"

# (description, compiled regex with ONE capture group = the vMAJOR.MINOR token).
# Each anchor matches a current-state label whose version MUST equal the
# declared MAJOR.MINOR.  Keep these tightly anchored so historical refs
# ("landed in v0.6") are never matched.
_ANCHORS: list[tuple[str, re.Pattern[str]]] = [
    ("intro badge — 'Partially silicon-verified (`vX.Y`)'",
     re.compile(r"Partially silicon-verified \(`v(\d+\.\d+)`\)")),
    ("Status heading — '**vX.Y ramp — paper-correct'",
     re.compile(r"\*\*v(\d+\.\d+) ramp — paper-correct")),
]


def declared_version() -> tuple[int, int, int]:
    """Return the (major, minor, patch) triple from sdk_version.yaml."""
    text = SDK_VERSION_YAML.read_text(encoding="utf-8")
    m = re.search(r"^version:\s*(\d+)\.(\d+)\.(\d+)\s*$", text, re.MULTILINE)
    if not m:
        print(f"check_version_doc_sync: could not parse 'version:' from "
              f"{SDK_VERSION_YAML.relative_to(REPO)}", file=sys.stderr)
        sys.exit(2)
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def check_readme(want_minor: str) -> list[str]:
    """Check the README current-state prose labels (MAJOR.MINOR only)."""
    readme = README.read_text(encoding="utf-8")
    drifts: list[str] = []
    for desc, rx in _ANCHORS:
        found = rx.search(readme)
        if found is None:
            drifts.append(f"  MISSING  {desc}: anchor not found in README.md "
                          f"(was it reworded? update scripts/check_version_doc_sync.py)")
            continue
        got = found.group(1)
        if got != want_minor:
            drifts.append(f"  STALE    {desc}: README says v{got}, "
                          f"sdk_version.yaml declares v{want_minor}")
    return drifts


def check_version_h(want: tuple[int, int, int]) -> list[str]:
    """Check include/alp/version.h's ALP_VERSION_* macros (full triple).

    Keep the parsers in lockstep with scripts/bump_version.py's
    update_version_h() rewrite patterns.
    """
    rel = VERSION_H.relative_to(REPO)
    text = VERSION_H.read_text(encoding="utf-8")
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


def check_pyproject(want: tuple[int, int, int]) -> list[str]:
    """Check pyproject.toml's [project] version (full triple)."""
    rel = PYPROJECT.relative_to(REPO)
    text = PYPROJECT.read_text(encoding="utf-8")
    want_str = ".".join(str(p) for p in want)
    m = re.search(r'^version\s*=\s*"([^"]*)"', text, re.MULTILINE)
    if m is None:
        return [f"  MISSING  {rel}: no 'version = \"...\"' line"]
    if m.group(1) != want_str:
        return [f"  STALE    {rel}: version = \"{m.group(1)}\", "
                f"sdk_version.yaml declares \"{want_str}\""]
    return []


def main() -> int:
    want = declared_version()
    want_minor = f"{want[0]}.{want[1]}"
    drifts = check_readme(want_minor) + check_version_h(want) + check_pyproject(want)

    if drifts:
        print(f"Version copies out of sync with metadata/sdk_version.yaml "
              f"(v{'.'.join(str(p) for p in want)}):", file=sys.stderr)
        print("\n".join(drifts), file=sys.stderr)
        print("\nThe release/version bump must update every version copy "
              "(scripts/bump_version.py does; historical 'landed in vX' README "
              "refs stay). -- failing.", file=sys.stderr)
        return 1

    print(f"check_version_doc_sync: OK (README labels, include/alp/version.h, "
          f"pyproject.toml all match v{'.'.join(str(p) for p in want)}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
