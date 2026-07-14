#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Validate a build plan against the public alp CLI / alp-sdk-vscode contract
(metadata/schemas/build-plan-v1.schema.json).

`--emit build-plan` (`scripts/alp_orchestrate/buildplan.py::emit_build_plan`)
renders one machine-readable JSON build plan per resolved board.yaml project:
one entry per non-off core slice with its build command, config artefact
contents, and app source dir, plus the shared (cross-slice) generated
artefacts and any non-fatal planning warnings. The `alp` CLI / alp-sdk-vscode
'Wave C' consumer reads THIS instead of re-deriving folder layout and build
wiring from board.yaml + the SoM presets.

With no --plan, this regenerates a plan from a representative set of example
projects via the orchestrator and validates each -- a conformance gate that
keeps `scripts/alp_orchestrate/`'s emitter and this contract in lockstep.
With --plan PATH it validates an existing plan (e.g. a real build plan an IDE
consumes).

Run locally:

    python3 scripts/check_build_plan.py
    python3 scripts/check_build_plan.py --plan build-plan.json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import jsonschema

REPO = Path(__file__).resolve().parent.parent
SCHEMA = REPO / "metadata" / "schemas" / "build-plan-v1.schema.json"
sys.path.insert(0, str(REPO / "scripts"))

# Representative projects exercising the multi-image (A+M) shape across all
# three SoC families -- the same corpus check_system_manifest.py pins.
_DEFAULT_PROJECTS = [
    "examples/multicore/rpmsg-v2n/board.yaml",
    "examples/multicore/rpmsg-aen/board.yaml",
    "examples/multicore/rpmsg-imx93/board.yaml",
    "examples/multicore/heterogeneous-offload/board.yaml",
]


def _make_validator(schema_path: Path) -> jsonschema.Draft202012Validator:
    schema = json.loads(Path(schema_path).read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator.check_schema(schema)
    return jsonschema.Draft202012Validator(
        schema, format_checker=jsonschema.FormatChecker())


def _validate_doc(label: str, doc, validator: jsonschema.Draft202012Validator) -> int:
    errors = sorted(validator.iter_errors(doc), key=lambda e: list(e.absolute_path))
    if errors:
        print(f"FAIL {label}")
        for err in errors:
            loc = "/".join(str(p) for p in err.absolute_path) or "<root>"
            print(f"  · {loc}: {err.message}")
        return 1
    print(f"OK   {label}")
    return 0


def _validate_file(path: Path, validator: jsonschema.Draft202012Validator) -> int:
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"FAIL {path.name}: parse error ({e})")
        return 1
    return _validate_doc(path.name, doc, validator)


def _validate_generated(board_yaml: Path, validator: jsonschema.Draft202012Validator) -> int:
    from alp_orchestrate import (
        OrchestratorError,
        emit_build_plan,
        load_board_yaml,
    )
    try:
        plan_json = emit_build_plan(
            load_board_yaml(board_yaml), board_yaml=board_yaml,
            build_root=REPO / "build")
    except OrchestratorError as e:
        print(f"FAIL {board_yaml}: {e}")
        return 1
    try:
        rel = board_yaml.relative_to(REPO)
    except ValueError:
        rel = board_yaml
    return _validate_doc(f"{rel} (generated)", json.loads(plan_json), validator)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Validate a build plan against the v1 contract.")
    ap.add_argument("--plan", type=Path, action="append", default=[],
                    help="build-plan JSON file(s) to validate. Default: "
                         "regenerate from representative example projects "
                         "and validate.")
    ap.add_argument("--schema", type=Path, default=SCHEMA)
    args = ap.parse_args()

    validator = _make_validator(args.schema)
    if args.plan:
        targets = args.plan
        failures = sum(_validate_file(p, validator) for p in targets)
    else:
        targets = [REPO / p for p in _DEFAULT_PROJECTS]
        failures = sum(_validate_generated(p, validator) for p in targets)
    print(f"\n{len(targets)} plan(s) checked, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
