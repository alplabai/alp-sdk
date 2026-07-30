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

THIS GATE NO LONGER GENERATES A PLAN TO CHECK.

It used to call the orchestrator's emitter directly on a representative set
of example projects and validate the fresh output -- proof that a plan built
*right now* still matched the contract. `scripts/alp_orchestrate/` is being
deleted (the planner it fronted now lives in the tan repository), so with no
--plan this gate instead validates the COMMITTED build-plan snapshots under
`tests/fixtures/emit-snapshots/*.build-plan.snap` -- generated output already
checked into the tree, the same artefact class `check_emit_registry.py`
leans on for the same reason.

WHAT IS LOST: this no longer proves a FRESHLY EMITTED plan conforms to the
schema -- only that the committed corpus does. If the corpus goes stale (an
emitter/tan change lands without regenerated snapshots), this gate stays
green on the old shape. Freshness is tan's responsibility now; it is the
thing that actually emits a build plan today.

With --plan PATH it validates an existing plan (e.g. a real build plan an IDE
consumes) -- unchanged.

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
SNAPSHOT_GLOB = "*.build-plan.snap"
DEFAULT_SNAPSHOT_DIR = REPO / "tests" / "fixtures" / "emit-snapshots"


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


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Validate a build plan against the v1 contract.")
    ap.add_argument("--plan", type=Path, action="append", default=[],
                    help="build-plan JSON file(s) to validate. Default: "
                         "validate the committed corpus under "
                         "tests/fixtures/emit-snapshots/*.build-plan.snap.")
    ap.add_argument("--schema", type=Path, default=SCHEMA)
    ap.add_argument("--snapshot-dir", type=Path, default=DEFAULT_SNAPSHOT_DIR,
                    help="Directory to glob %s from when --plan is not "
                         "given (default: tests/fixtures/emit-snapshots)."
                         % SNAPSHOT_GLOB)
    args = ap.parse_args()

    validator = _make_validator(args.schema)
    if args.plan:
        targets = args.plan
    else:
        targets = sorted(args.snapshot_dir.glob(SNAPSHOT_GLOB))
        if not targets:
            print(f"FAIL no build-plan snapshots matched "
                  f"{args.snapshot_dir}/{SNAPSHOT_GLOB} -- an empty corpus "
                  f"checks nothing, which is the failure class this gate "
                  f"exists to catch, not a pass")
            return 1

    failures = sum(_validate_file(p, validator) for p in targets)
    print(f"\n{len(targets)} plan(s) checked, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
