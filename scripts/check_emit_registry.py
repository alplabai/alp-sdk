#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Validate metadata/emit-registry-v1.json against the public IDE/tool contract
(metadata/schemas/emit-registry-v1.schema.json) AND assert its mode set is
exactly the real `--emit` surface the SDK exposes.

The registry (metadata/emit-registry-v1.json) is the single source of truth
for every `--emit` mode across scripts/alp_project.py and
scripts/alp_orchestrate/cli.py (re-exposed by scripts/alp_cli/emit.py /
`west alp-emit`). Before this gate, that mode set was hand-mirrored in three
places (alp_project.py's argparse choices + dispatch, alp_orchestrate's
choices, and alp_cli/emit.py's two "Mirror ..." lists) with nothing catching
drift. This gate parses the REAL `--emit` choices out of the two CLI source
files (ast, not a hardcoded list -- so it can't itself drift) and fails if:

  - a mode is in the code but missing from the registry, or
  - a mode is in the registry but not in the code (a phantom entry).

Run locally:

    python3 scripts/check_emit_registry.py
"""

from __future__ import annotations

import argparse
import ast
import json
import sys
from pathlib import Path

import jsonschema

REPO = Path(__file__).resolve().parent.parent
REGISTRY = REPO / "metadata" / "emit-registry-v1.json"
SCHEMA = REPO / "metadata" / "schemas" / "emit-registry-v1.schema.json"
ALP_PROJECT = REPO / "scripts" / "alp_project.py"
ALP_ORCHESTRATE_CLI = REPO / "scripts" / "alp_orchestrate" / "cli.py"


def _emit_choices(source: Path) -> set[str]:
    """The literal `choices=[...]` of the `--emit` argparse argument in
    *source*, discovered via ast (never hardcoded, so this can't itself
    drift from the CLI it inspects)."""
    tree = ast.parse(source.read_text(encoding="utf-8"))
    for node in ast.walk(tree):
        if not (isinstance(node, ast.Call)
                and any(isinstance(a, ast.Constant) and a.value == "--emit"
                        for a in node.args)):
            continue
        for kw in node.keywords:
            if kw.arg == "choices" and isinstance(kw.value, (ast.List, ast.Tuple)):
                return {e.value for e in kw.value.elts
                        if isinstance(e, ast.Constant)}
    raise SystemExit(
        f"check_emit_registry: could not find a `--emit ... choices=[...]` "
        f"argparse argument in {source.relative_to(REPO).as_posix()}")


def real_emit_modes() -> set[str]:
    """The complete, de-duplicated `--emit` mode set the SDK actually
    exposes: alp_project.py's choices union alp_orchestrate's choices
    (mirrors scripts/alp_cli/emit.py's EMIT_MODES construction)."""
    return _emit_choices(ALP_PROJECT) | _emit_choices(ALP_ORCHESTRATE_CLI)


def _make_validator(schema_path: Path) -> jsonschema.Draft202012Validator:
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator.check_schema(schema)
    return jsonschema.Draft202012Validator(
        schema, format_checker=jsonschema.FormatChecker())


def _validate_schema(doc: dict, validator: jsonschema.Draft202012Validator) -> list[str]:
    errors = sorted(validator.iter_errors(doc), key=lambda e: list(e.absolute_path))
    out = []
    for err in errors:
        loc = "/".join(str(p) for p in err.absolute_path) or "<root>"
        out.append(f"{loc}: {err.message}")
    return out


def _check_mode_drift(doc: dict) -> list[str]:
    registry_modes = {m["mode"] for m in doc.get("modes", [])}
    code_modes = real_emit_modes()

    missing = sorted(code_modes - registry_modes)
    phantom = sorted(registry_modes - code_modes)

    problems = []
    if missing:
        problems.append(
            "modes present in the code's --emit choices but missing from "
            f"the registry: {missing}")
    if phantom:
        problems.append(
            "modes present in the registry but not in any CLI's --emit "
            f"choices (phantom entries): {phantom}")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--registry", type=Path, default=REGISTRY)
    ap.add_argument("--schema", type=Path, default=SCHEMA)
    args = ap.parse_args()

    try:
        label = args.registry.relative_to(REPO).as_posix()
    except ValueError:
        label = str(args.registry)

    try:
        doc = json.loads(args.registry.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"FAIL {label}: parse error ({e})")
        return 1

    validator = _make_validator(args.schema)
    problems = _validate_schema(doc, validator)
    problems += _check_mode_drift(doc)

    if problems:
        print(f"FAIL {label}")
        for p in problems:
            print(f"  · {p}")
        return 1

    print(f"OK   {label}  "
          f"({len(doc['modes'])} emit modes, in sync with the code)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
