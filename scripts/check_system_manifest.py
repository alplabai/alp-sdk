#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Validate a system manifest against the public IDE/tool contract
(metadata/schemas/system-manifest-v1.schema.json).

`build/system-manifest.yaml` is the single derived projection of a
board.yaml that `west alp-build` emits: one entry per per-core image with
its runtime + build/flash wiring. Tools (the alp-sdk-vscode extension, CI,
flashers) read THIS instead of re-deriving folder layout and build wiring
from board.yaml + the SoM presets.

With no --manifest, this regenerates a manifest from a representative set of
example projects via the orchestrator and validates each -- a conformance
gate that keeps scripts/alp_orchestrate.py's emitter and this contract in
lockstep. With --manifest PATH it validates an existing manifest (e.g. a real
build/system-manifest.yaml an IDE consumes).

Run locally:

    python3 scripts/check_system_manifest.py
    python3 scripts/check_system_manifest.py --manifest build/system-manifest.yaml
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import jsonschema
import yaml

REPO = Path(__file__).resolve().parent.parent
SCHEMA = REPO / "metadata" / "schemas" / "system-manifest-v1.schema.json"
sys.path.insert(0, str(REPO / "scripts"))

# Representative projects exercising the multi-image (A+M) and single-image
# shapes across all three SoC families.
_DEFAULT_PROJECTS = [
    "examples/multicore/rpmsg-v2n/board.yaml",
    "examples/multicore/rpmsg-aen/board.yaml",
    "examples/multicore/rpmsg-imx93/board.yaml",
    "examples/multicore/heterogeneous-offload/board.yaml",
    "examples/peripheral-io/i2c-scanner/board.yaml",
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
        doc = yaml.safe_load(path.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"FAIL {path.name}: parse error ({e})")
        return 1
    return _validate_doc(path.name, doc, validator)


def _validate_generated(board_yaml: Path, validator: jsonschema.Draft202012Validator) -> int:
    from alp_orchestrate import (
        OrchestratorError,
        emit_system_manifest,
        load_board_yaml,
    )
    try:
        manifest_yaml = emit_system_manifest(load_board_yaml(board_yaml))
    except OrchestratorError as e:
        print(f"FAIL {board_yaml}: {e}")
        return 1
    try:
        rel = board_yaml.relative_to(REPO)
    except ValueError:
        rel = board_yaml
    return _validate_doc(f"{rel} (generated)", yaml.safe_load(manifest_yaml), validator)


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Validate system-manifest.yaml against the v1 contract.")
    ap.add_argument("--manifest", type=Path, action="append", default=[],
                    help="manifest file(s) to validate. Default: regenerate "
                         "from representative example projects and validate.")
    ap.add_argument("--schema", type=Path, default=SCHEMA)
    args = ap.parse_args()

    validator = _make_validator(args.schema)
    if args.manifest:
        targets = args.manifest
        failures = sum(_validate_file(p, validator) for p in targets)
    else:
        targets = [REPO / p for p in _DEFAULT_PROJECTS]
        failures = sum(_validate_generated(p, validator) for p in targets)
    print(f"\n{len(targets)} manifest(s) checked, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
