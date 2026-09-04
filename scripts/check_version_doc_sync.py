#!/usr/bin/env python3
"""Fail if any version copy drifts from the declared SDK version.

The release flow (`scripts/bump_version.py`, the `cutting-a-release` skill)
updates every machine-read place that tracks the version; this check makes
any miss a CI failure instead of a silent drift.  Verified copies:

  1. include/alp/version.h -- ALP_VERSION_MAJOR/MINOR/PATCH (core triple)
     and the ALP_VERSION_STRING literal, which may carry the SAME SemVer
     pre-release suffix (`0.16.0-rc1`) sdk_version.yaml's `version:` does
     during an rc window (#1902) -- checked against the FULL declared
     string, not just the core triple every other copy below uses.
  2. pyproject.toml -- the alp-sdk-cli `[project]` version (core
     MAJOR.MINOR.PATCH, never a pre-release suffix -- PEP 440 doesn't
     accept one; sat stale at 0.6.0 until v0.8.1).
  3. src/zephyr/alp_banner.c -- the sample banner line in the file's
     doc-comment (core triple).  The banner *code* always prints the live
     ALP_VERSION_STRING (suffix included); only the illustrative comment
     stays pinned to the core version and can drift.
  4. VERSIONS.md -- the living roadmap ledger must carry a `| vMAJOR.MINOR.PATCH`
     table row for the declared (core) version (issue #1213/#1199: "existing
     green gates miss ... stale ledgers" -- a version bump with no matching
     VERSIONS.md row previously stayed green here).  Only row EXISTENCE is
     checked, never its prose content -- VERSIONS.md's per-version summary is
     free-form and reviewed by hand, same as every other row already in the
     table.

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


_VERSION_LINE_RE = re.compile(r"^version:\s*(\d+)\.(\d+)\.(\d+)(?:-([\w.]+))?\s*$", re.MULTILINE)


def _declared_match(repo: pathlib.Path) -> re.Match[str]:
    sdk_version_yaml = repo / "metadata" / "sdk_version.yaml"
    text = sdk_version_yaml.read_text(encoding="utf-8")
    m = _VERSION_LINE_RE.search(text)
    if not m:
        print(f"check_version_doc_sync: could not parse 'version:' from "
              f"{sdk_version_yaml.relative_to(repo).as_posix()}", file=sys.stderr)
        sys.exit(2)
    return m


def declared_version(repo: pathlib.Path) -> tuple[int, int, int]:
    """Return the (major, minor, patch) CORE triple from sdk_version.yaml.

    Any SemVer pre-release suffix (`0.16.0-rc1`, #1902) is accepted but
    dropped here -- ALP_VERSION_MAJOR/MINOR/PATCH, pyproject.toml,
    alp_banner.c and VERSIONS.md all stay pinned to the plain core triple
    even during an rc window (see scripts/bump_version.py); only
    ALP_VERSION_STRING carries the suffix, checked separately by
    `declared_version_full()`.
    """
    m = _declared_match(repo)
    return int(m.group(1)), int(m.group(2)), int(m.group(3))


def declared_version_full(repo: pathlib.Path) -> str:
    """Return the FULL declared version string, suffix included."""
    m = _declared_match(repo)
    core = f"{m.group(1)}.{m.group(2)}.{m.group(3)}"
    return f"{core}-{m.group(4)}" if m.group(4) else core


def check_version_h(repo: pathlib.Path, want: tuple[int, int, int], want_full: str) -> list[str]:
    """Check include/alp/version.h's ALP_VERSION_* macros.

    ALP_VERSION_MAJOR/MINOR/PATCH check against the CORE triple (`want`);
    ALP_VERSION_STRING checks against `want_full`, which carries the same
    SemVer pre-release suffix sdk_version.yaml's `version:` does, if any
    (#1902) -- a bare `want`-only compare would flag every legitimate rc
    build's ALP_VERSION_STRING as drift.

    Keep the parsers in lockstep with scripts/bump_version.py's
    update_version_h() rewrite patterns.
    """
    version_h = repo / "include" / "alp" / "version.h"
    rel = version_h.relative_to(repo).as_posix()
    text = version_h.read_text(encoding="utf-8")
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
    elif m.group(1) != want_full:
        drifts.append(f"  STALE    {rel}: ALP_VERSION_STRING is \"{m.group(1)}\", "
                      f"sdk_version.yaml declares \"{want_full}\"")
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


def check_versions_md(repo: pathlib.Path, want_str: str) -> list[str]:
    """Check VERSIONS.md carries a roadmap table row for the declared version.

    Matches a literal `| vMAJOR.MINOR.PATCH` table-row cell (the shape every
    existing row uses, e.g. `| v0.15.0 |`).  Content is NOT checked -- only
    that a release doesn't go completely unlisted, the way #1199 found
    VERSIONS.md sitting multiple releases behind while every other version
    copy this gate already checks stayed in sync.
    """
    versions_md = repo / "VERSIONS.md"
    rel = versions_md.relative_to(repo).as_posix()
    if not versions_md.is_file():
        return [f"  MISSING  {rel}: file not found"]
    text = versions_md.read_text(encoding="utf-8")
    row_re = re.compile(rf"^\|\s*v{re.escape(want_str)}\s*\|", re.MULTILINE)
    if row_re.search(text) is None:
        return [f"  MISSING  {rel}: no '| v{want_str}' roadmap table row for "
                f"the version metadata/sdk_version.yaml declares"]
    return []


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1] if __doc__ else "")
    ap.add_argument("--root", default=None,
                     help="Repo root to check (default: this script's repo)")
    args = ap.parse_args()

    repo = pathlib.Path(args.root).resolve() if args.root else pathlib.Path(__file__).resolve().parent.parent

    want = declared_version(repo)
    want_str = ".".join(str(p) for p in want)
    want_full = declared_version_full(repo)

    drifts = (
        check_version_h(repo, want, want_full)
        + check_pyproject(repo, want_str)
        + check_banner_c(repo, want_str)
        + check_versions_md(repo, want_str)
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
          f"src/zephyr/alp_banner.c all match v{want_str}; VERSIONS.md carries "
          f"a v{want_str} row).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
