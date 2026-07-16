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
Fast, filesystem-only.
"""
from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve()
sys.path.insert(0, str(_HERE.parent))
import alp_migrate  # noqa: E402

ROOT = _HERE.parent.parent


def find_drift(root: Path) -> list[Path]:
    drifted: list[Path] = []
    for path in sorted(root.rglob("board.yaml")):
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
