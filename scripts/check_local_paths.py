#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: no hard-coded home-directory paths in executable / build files.

Checked-in scripts, firmware, examples, CMake, and config files must not
embed an absolute maintainer path like ``/home/<user>/...`` (issue #516).
Such paths make the SDK fail on any other checkout, CI job, or customer
workstation, can consume artifacts from the wrong tree, and leak the
maintainer's local layout into customer-facing files.  Use required env
vars, CLI flags, or paths derived from the script/repo root instead, and
placeholders such as ``<alp-sdk>`` / ``<zephyr-workspace>`` / ``<ti-sdk>``
in comments.

Markdown docs are intentionally NOT scanned here -- ``check_cross_platform.py``
already flags forward-slash home paths in docs (per ADR 0012).

Suppression: add a path (relative to the repo root) to ``ALLOWLIST`` with a
one-line reason.  Reserve it for files that legitimately document the
pattern (e.g. the cross-platform linter and its tests) or archival notes.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# A home directory followed by a path separator: /home/<user>/...
HOME_PATH_RE = re.compile(r"/home/[A-Za-z0-9._-]+/")

# Extensions / names that are executable or build inputs (not prose docs).
SCANNED_SUFFIXES = {
    ".sh", ".ps1", ".bat", ".cmake", ".conf", ".cfg", ".ini",
    ".c", ".h", ".cpp", ".hpp", ".py", ".yaml", ".yml", ".overlay", ".dts", ".dtsi",
}
SCANNED_NAMES = {"CMakeLists.txt", "Makefile", "Kconfig"}

# path (repo-relative) -> reason. Files that legitimately contain a home path.
ALLOWLIST: dict[str, str] = {
    "scripts/check_cross_platform.py": "documents the /home/user pattern it lints",
    "scripts/check_local_paths.py": "this linter documents the pattern in its docstring",
    "tests/scripts/test_check_cross_platform.py": "test fixtures exercise the pattern",
    "tests/scripts/test_check_emit_snapshots.py": "test fixtures feed /home paths to the SDK-root normalizer under test",
}

# Directory prefixes that are archival and exempt (mirrors the git grep filter
# the issue used: docs/superpowers/plans is captured planning history).
EXEMPT_PREFIXES = ("docs/superpowers/plans/",)


def _tracked_files(root: Path) -> list[str]:
    out = subprocess.run(
        ["git", "-C", str(root), "ls-files"],
        capture_output=True, text=True, check=True,
    )
    return out.stdout.splitlines()


def _is_scanned(rel: str) -> bool:
    p = Path(rel)
    return p.suffix in SCANNED_SUFFIXES or p.name in SCANNED_NAMES


def check(root: Path) -> list[str]:
    offenders: list[str] = []
    for rel in _tracked_files(root):
        if rel in ALLOWLIST:
            continue
        if any(rel.startswith(pfx) for pfx in EXEMPT_PREFIXES):
            continue
        if not _is_scanned(rel):
            continue
        text = (root / rel).read_text(encoding="utf-8", errors="replace")
        for i, line in enumerate(text.splitlines(), start=1):
            if HOME_PATH_RE.search(line):
                offenders.append(f"{rel}:{i}: hard-coded home path: {line.strip()}")
    return offenders


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    offenders = check(args.root)
    if not offenders:
        return 0

    print("check_local_paths: hard-coded home-directory paths found "
          "(use env vars / flags / script-relative paths or placeholders):")
    for o in offenders:
        print(f"  {o}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
