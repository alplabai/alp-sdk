#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Assert the library alias table covers every curated token and resolves to
real manifests (WS6-c #610 §6).

The consolidation (per-core `libraries:` enum -> the manifest model) needs a
complete, honest bridge before any resolver rewrite. This gate makes the
coverage gap machine-visible:

  - metadata/library-aliases-v1.json validates against its schema;
  - every curated per-core token (the board.schema.json `cores.<id>.libraries`
    enum) has an alias entry;
  - every alias value names an existing metadata/libraries/<name>.yaml manifest
    (no dangling canonical name).

stdlib + jsonschema (+ yaml only to confirm a manifest parses).
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import jsonschema

_HERE = Path(__file__).resolve()
ROOT = _HERE.parent.parent


def _curated_tokens(root: Path) -> set[str]:
    """The per-core `libraries` enum from board.schema.json."""
    schema = json.loads((root / "metadata/schemas/board.schema.json").read_text(encoding="utf-8"))
    found: set[str] = set()

    def walk(o):
        if isinstance(o, dict):
            for k, v in o.items():
                if k == "libraries" and isinstance(v, dict):
                    items = v.get("items", {})
                    if isinstance(items, dict) and "enum" in items:
                        found.update(items["enum"])
                walk(v)
        elif isinstance(o, list):
            for v in o:
                walk(v)

    walk(schema)
    return found


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    alias_p = root / "metadata/library-aliases-v1.json"
    schema_p = root / "metadata/schemas/library-aliases-v1.schema.json"
    if not alias_p.is_file() or not schema_p.is_file():
        return [f"missing {alias_p if not alias_p.is_file() else schema_p}"]
    schema = json.loads(schema_p.read_text(encoding="utf-8"))
    table = json.loads(alias_p.read_text(encoding="utf-8"))
    try:
        jsonschema.Draft202012Validator(schema).validate(table)
    except jsonschema.ValidationError as e:
        return [f"alias schema: {e.message}"]
    aliases = table["aliases"]

    tokens = _curated_tokens(root)
    for tok in sorted(tokens - set(aliases)):
        problems.append(f"curated token '{tok}' has no alias entry")
    libdir = root / "metadata" / "libraries"
    for tok, canonical in sorted(aliases.items()):
        if not (libdir / f"{canonical}.yaml").is_file():
            problems.append(
                f"alias '{tok}' -> '{canonical}' names no metadata/libraries/{canonical}.yaml")
    return problems


def main() -> int:
    problems = find_problems(ROOT)
    if problems:
        for p in problems:
            print(f"library-registry: {p}", file=sys.stderr)
        return 1
    print("OK: library alias table covers every curated token, all resolve to manifests.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
