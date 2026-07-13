#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fail if any repo board.yaml is not at the canonical schema version.

Every board.yaml tracked in the repo must carry an explicit, current
`schemaVersion` (epic #610 WS6-b). Absent or older ones are drift -- run
`west alp-migrate --apply` to bring them up. Fast, filesystem-only.
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
        if doc is not None and alp_migrate.plan(doc):
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
