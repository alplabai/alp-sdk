#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fail if any repo board.yaml has an outstanding schema migration.

Lazy versioning (epic #610 WS6-b): an absent `schemaVersion` IS version 1
(the floor), so absent and at-`LATEST` files are clean -- not drift. A file
is drift only when a registered migration would advance it (`plan()`
non-empty). While the migration registry is empty this gate is a no-op; it
gains teeth automatically once a v1->v2 migration lands. A file whose
`schemaVersion` is NEWER than this SDK's `LATEST` is also reported (it cannot
be migrated down). Run `west alp-migrate --apply` to resolve real drift.
Fast: board.yaml discovery goes through `git ls-files` (prunes build output
via `.gitignore` without descending into it), not a raw filesystem walk.
"""
from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

_HERE = Path(__file__).resolve()
sys.path.insert(0, str(_HERE.parent))
import alp_migrate  # noqa: E402

ROOT = _HERE.parent.parent

# Fallback-only: dirs to prune when `root` isn't a git worktree (see
# `_board_yaml_files`). git itself needs no such list -- `--exclude-
# standard` already reads `.gitignore`, the one source of truth for
# "this is build output, not a source".
_FALLBACK_SKIP_DIRS = frozenset({".git", ".west", "build", "twister-out"})


def _board_yaml_files(root: Path) -> list[Path]:
    """Every board.yaml under `root` -- tracked or newly-created-but-not-
    yet-`git add`ed. Same fix as check_library_registry.py's
    `_board_yaml_files`, same defect class: a raw `root.rglob("board.yaml")`
    here also walked `twister-out/`/`build/` unbounded from repo root."""
    try:
        proc = subprocess.run(
            ["git", "-C", str(root), "ls-files", "--cached", "--others",
             "--exclude-standard", "--", "*board.yaml"],
            check=True, capture_output=True, text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        found: list[Path] = []
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = [d for d in dirnames if d not in _FALLBACK_SKIP_DIRS]
            if "board.yaml" in filenames:
                found.append(Path(dirpath) / "board.yaml")
        return sorted(found)
    return sorted(root / line for line in proc.stdout.splitlines() if line)


def find_drift(root: Path) -> list[Path]:
    drifted: list[Path] = []
    for path in _board_yaml_files(root):
        try:
            doc = alp_migrate.load(path.read_text(encoding="utf-8"))
        except Exception:
            continue  # not our concern; other gates validate board.yaml shape
        if doc is None:
            continue
        try:
            if alp_migrate.plan(doc):
                drifted.append(path)
        except alp_migrate.MigrateError as e:
            # schemaVersion newer than LATEST -- a real problem, but report it
            # cleanly like the CLI does instead of tracebacking the gate.
            print(f"board-schema-version: {path}: {e}", file=sys.stderr)
            drifted.append(path)
    return drifted


def main() -> int:
    drifted = find_drift(ROOT)
    if drifted:
        for p in drifted:
            print(f"board-schema-version: {p.relative_to(ROOT)} needs "
                  f"`west alp-migrate --apply` (not at v{alp_migrate.LATEST})",
                  file=sys.stderr)
        return 1
    print(f"OK: all board.yaml at schemaVersion {alp_migrate.LATEST}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
