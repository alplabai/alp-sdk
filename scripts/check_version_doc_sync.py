#!/usr/bin/env python3
"""Fail if any version copy drifts from the declared SDK version.

The release flow (`scripts/bump_version.py`, the `cutting-a-release` skill)
updates every machine-read place that tracks the version; this check makes
any miss a CI failure instead of a silent drift.  Verified copies:

  1. include/alp/version.h -- the ALP_VERSION_MAJOR/MINOR/PATCH macros and
     the ALP_VERSION_STRING literal (full MAJOR.MINOR.PATCH).
  2. pyproject.toml -- the alp-sdk-cli `[project]` version (full
     MAJOR.MINOR.PATCH; sat stale at 0.6.0 until v0.8.1).
  3. src/zephyr/alp_banner.c -- the sample banner line in the file's
     doc-comment (full triple).  The banner *code* always prints the live
     ALP_VERSION_STRING; only the illustrative comment can drift.

The README / docs current-state prose is de-versioned: its status lines were
rewritten to carry no version label ("Partially silicon-verified", "Current
ramp"), so there is nothing to sync there.  scripts/alp_cli/__init__.py
derives `__version__` from metadata/sdk_version.yaml at import time (no
literal to drift).  Historical / narrative `v0.x` references across the tree
("the silicon-verified slice landed in v0.6", "verified v0.8", "SE_RESET
(v0.8)", the DIFFERENT GD32-bridge-protocol version, ADRs, frozen
docs/abi/vX.Y-snapshot.json files) describe what happened AT a past version
and are correct as-is -- they must stay untouched.

Authoritative version: `metadata/sdk_version.yaml` (`version: MAJOR.MINOR.PATCH`).

Exit 0 = in sync, 1 = drift (with the offending lines), 2 = setup error.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys


def declared_version(repo: pathlib.Path) -> tuple[int, int, int]:
    """Return the (major, minor, patch) triple from sdk_version.yaml."""
    sdk_version_yaml = repo / "metadata" / "sdk_version.yaml"
    text = sdk_version_yaml.read_text(encoding="utf-8")
    m = re.search(r"^version:\s*(\d+)\.(\d+)\.(\d+)\s*$", text, re.MULTILINE)
    if not m:
        print(f"check_version_doc_sync: could not parse 'version:' from "
              f"{sdk_version_yaml.relative_to(repo).as_posix()}", file=sys.stderr)
        sys.exit(2)
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def check_version_h(repo: pathlib.Path, want: tuple[int, int, int]) -> list[str]:
    """Check include/alp/version.h's ALP_VERSION_* macros (full triple).

    Keep the parsers in lockstep with scripts/bump_version.py's
    update_version_h() rewrite patterns.
    """
    version_h = repo / "include" / "alp" / "version.h"
    rel = version_h.relative_to(repo).as_posix()
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
    rel = pyproject.relative_to(repo).as_posix()
    text = pyproject.read_text(encoding="utf-8")
    m = re.search(r'^version\s*=\s*"([^"]*)"', text, re.MULTILINE)
    if m is None:
        return [f"  MISSING  {rel}: no 'version = \"...\"' line"]
    if m.group(1) != want_str:
        return [f"  STALE    {rel}: version = \"{m.group(1)}\", "
                f"sdk_version.yaml declares \"{want_str}\""]
    return []


def check_banner_c(repo: pathlib.Path, want_str: str) -> list[str]:
    """Check src/zephyr/alp_banner.c's sample banner line (full triple).

    The banner *code* prints the live ALP_VERSION_STRING at runtime; only
    the illustrative sample line in the file's doc-comment can drift.
    """
    banner_c = repo / "src" / "zephyr" / "alp_banner.c"
    rel = banner_c.relative_to(repo).as_posix()
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
    want_str = ".".join(str(p) for p in want)

    drifts = (
        check_version_h(repo, want)
        + check_pyproject(repo, want_str)
        + check_banner_c(repo, want_str)
    )

    if drifts:
        print(f"Version copies out of sync with metadata/sdk_version.yaml "
              f"(v{want_str}):", file=sys.stderr)
        print("\n".join(drifts), file=sys.stderr)
        print("\nThe release/version bump must update every machine-read version "
              "copy (scripts/bump_version.py does; de-versioned README prose and "
              "historical 'landed in vX' refs stay). -- failing.", file=sys.stderr)
        return 1

    print(f"check_version_doc_sync: OK (include/alp/version.h, pyproject.toml, "
          f"src/zephyr/alp_banner.c all match v{want_str}).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
