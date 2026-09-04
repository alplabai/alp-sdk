#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Validate a build plan against the public alp CLI / alp-sdk-vscode contract
(metadata/schemas/build-plan-v1.schema.json), plus, for a plan the SDK
emits itself, the `command.tool` bare-identity convention the shared
schema deliberately does not assert (#1286).

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
import re
import sys
from pathlib import Path

import jsonschema

REPO = Path(__file__).resolve().parent.parent
SCHEMA = REPO / "metadata" / "schemas" / "build-plan-v1.schema.json"
sys.path.insert(0, str(REPO / "scripts"))

from alp_orchestrate.sdk_compat import assert_exclusion_still_not_buildable  # noqa: E402

# Issue #1286: `command.tool` must be a bare executable identity (e.g.
# 'west'), never a location -- the executor's job is resolving that
# identity to a concrete path, not the plan's. This is asserted ONLY over
# plans WE generate (see _validate_generated below), never the shared
# schema (metadata/schemas/build-plan-v1.schema.json carries no `pattern`
# on this field on purpose): a third-party plan may legitimately carry an
# absolute tool path, and #847 already showed that tightening a shared
# shape at unchanged `schemaVersion: const 1` breaks pinned consumers.
# `re.fullmatch`, not `^...$`: JSON Schema is ECMA-262 where `$` matches
# only at end of input, while Python `re`'s `$` also matches just before a
# trailing newline -- `re.fullmatch` keeps this gate's verdict identical
# to what an ajv-based consumer would give.
_TOOL_IDENTITY_RE = re.compile(r"[A-Za-z0-9._+-]+")


def _tool_identity_violations(doc) -> list[str]:
    """Return one message per slice command whose `tool` isn't a bare
    executable identity. Doc-shaped input only -- callers own schema
    validation separately.

    Covers `postCommands[]` as well as `command` (alplabai/tan-cli#550):
    those steps are dispatched by the same executor under the same
    `executionPolicy`, so a location leaking into one of them is the
    identical #1286 defect.
    """
    bad = []
    for slice_ in doc.get("slices", []):
        steps = [("command", slice_.get("command"))]
        steps += [(f"postCommands[{i}]", step) for i, step
                  in enumerate(slice_.get("postCommands") or [])]
        for field, command in steps:
            if command is None:
                continue
            tool = command.get("tool", "")
            if (not isinstance(tool, str)
                    or not _TOOL_IDENTITY_RE.fullmatch(tool)):
                bad.append(
                    f"slices[{slice_.get('coreId')!r}].{field}.tool "
                    f"{tool!r} is not a bare executable identity "
                    f"(issue #1286)")
    return bad


# Representative projects exercising the multi-image (A+M) shape across all
# three SoC families -- the same corpus check_system_manifest.py pins.
#
# rpmsg-imx93 excluded (#1025): E1M-NX9101's only hw_rev (imx93 r1) is
# `status: tbd` -- refused outright by the hw_rev-buildable gate. Re-add
# "examples/multicore/rpmsg-imx93/board.yaml" once
# metadata/e1m_modules/imx93/hw-revisions.yaml:r1 carries a buildable status.
# `main()` re-asserts that reason still holds every run (RATCHET --
# see assert_exclusion_still_not_buildable).
_DEFAULT_PROJECTS = [
    "examples/multicore/rpmsg-v2n/board.yaml",
    "examples/multicore/rpmsg-aen/board.yaml",
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
    label = f"{rel} (generated)"
    doc = json.loads(plan_json)
    failures = _validate_doc(label, doc, validator)
    for msg in _tool_identity_violations(doc):
        print(f"FAIL {label}: {msg}")
        failures += 1
    return failures


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
        stale = assert_exclusion_still_not_buildable(
            REPO / "metadata", "imx93", "r1", gate="check_build_plan.py")
        if stale:
            print(f"FAIL {stale}", file=sys.stderr)
            failures += 1
    print(f"\n{len(targets)} plan(s) checked, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
