#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Keep metadata/quality-tasks-v1.json == the SDK's real quality gates.

Validates the registry against its schema and asserts every scripts/check_*.py
on disk is listed exactly once (no orphan gate, no phantom entry) -- the drift
#608 flagged. stdlib + jsonschema.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import jsonschema

_HERE = Path(__file__).resolve()
ROOT = _HERE.parent.parent


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    schema_p = root / "metadata/schemas/quality-tasks-v1.schema.json"
    reg_p = root / "metadata/quality-tasks-v1.json"
    if not reg_p.is_file() or not schema_p.is_file():
        return [f"missing {reg_p if not reg_p.is_file() else schema_p}"]
    schema = json.loads(schema_p.read_text(encoding="utf-8"))
    reg = json.loads(reg_p.read_text(encoding="utf-8"))
    try:
        jsonschema.Draft202012Validator(schema).validate(reg)
    except jsonschema.ValidationError as e:
        return [f"schema: {e.message}"]
    on_disk = {p.name for p in (root / "scripts").glob("check_*.py")
               if p.name != "check_quality_registry.py"}
    listed = [Path(t["script"]).name for t in reg["tasks"]
              if t.get("runner") == "check-script"]
    listed_set = set(listed)
    for orphan in sorted(on_disk - listed_set):
        problems.append(f"{orphan}: on disk but missing from quality-tasks-v1.json")
    for phantom in sorted(listed_set - on_disk):
        problems.append(f"{phantom}: in registry but no such scripts/ file")
    if len(listed) != len(listed_set):
        problems.append("duplicate check-script entries in registry")
    ids = [t["id"] for t in reg["tasks"]]
    dup_ids = sorted({i for i in ids if ids.count(i) > 1})
    for d in dup_ids:
        problems.append(f"duplicate task id: {d}")
    return problems


def main() -> int:
    problems = find_problems(ROOT)
    if problems:
        for p in problems:
            print(f"quality-registry: {p}", file=sys.stderr)
        return 1
    print("OK: quality-tasks-v1.json matches scripts/check_*.py.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
