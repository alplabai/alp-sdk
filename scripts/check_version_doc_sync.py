#!/usr/bin/env python3
"""Fail if any version copy drifts from the declared SDK version.

The release flow (`scripts/bump_version.py`, the `cutting-a-release` skill)
updates every place that tracks the version; this check makes any miss a CI
failure instead of a silent drift.  Verified copies:

  1. include/alp/version.h -- the ALP_VERSION_MAJOR/MINOR/PATCH macros and
     the ALP_VERSION_STRING literal (full MAJOR.MINOR.PATCH).
  2. pyproject.toml -- the alp-sdk-cli `[project]` version (full
     MAJOR.MINOR.PATCH; sat stale at 0.6.0 until v0.8.1).

The README prose no longer carries a current-state version label: its
status lines were de-versioned ("Partially silicon-verified", "Current
ramp") so there is nothing to sync there.  Historical references
("the silicon-verified slice landed in v0.6", "from v0.6 onward") are
past-tense facts and stay.  The only machine-read version copies left to
gate are the two above -- both rewritten by `scripts/bump_version.py`.

Authoritative version: `metadata/sdk_version.yaml` (`version: MAJOR.MINOR.PATCH`).

Exit 0 = in sync, 1 = drift (with the offending lines), 2 = setup error.
"""
from __future__ import annotations

import pathlib
import re
import sys

REPO = pathlib.Path(__file__).resolve().parent.parent
SDK_VERSION_YAML = REPO / "metadata" / "sdk_version.yaml"
VERSION_H = REPO / "include" / "alp" / "version.h"
PYPROJECT = REPO / "pyproject.toml"


def declared_version() -> tuple[int, int, int]:
    """Return the (major, minor, patch) triple from sdk_version.yaml."""
    text = SDK_VERSION_YAML.read_text(encoding="utf-8")
    m = re.search(r"^version:\s*(\d+)\.(\d+)\.(\d+)\s*$", text, re.MULTILINE)
    if not m:
        print(f"check_version_doc_sync: could not parse 'version:' from "
              f"{SDK_VERSION_YAML.relative_to(REPO)}", file=sys.stderr)
        sys.exit(2)
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


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
    drifts = check_version_h(want) + check_pyproject(want)

    if drifts:
        print(f"Version copies out of sync with metadata/sdk_version.yaml "
              f"(v{'.'.join(str(p) for p in want)}):", file=sys.stderr)
        print("\n".join(drifts), file=sys.stderr)
        print("\nThe release/version bump must update every version copy "
              "(scripts/bump_version.py does). -- failing.", file=sys.stderr)
        return 1

    print(f"check_version_doc_sync: OK (include/alp/version.h, "
          f"pyproject.toml match v{'.'.join(str(p) for p in want)}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
