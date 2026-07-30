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

WHAT WAS LOST: this gate used to REGENERATE a manifest from a representative
set of example board.yaml files via scripts/alp_orchestrate/'s emitter and
validate the fresh output -- a conformance gate that kept the emitter and
this schema in lockstep. scripts/alp_orchestrate/ is being deleted (the
planner it fronted now lives in the tan repository), so alp-sdk can no
longer produce a manifest to check. With no --manifest, this now validates
the COMMITTED corpus at tests/fixtures/emit-snapshots/*.system-manifest.snap
instead -- proof that corpus still conforms to the schema, NOT proof that a
freshly emitted manifest does. Freshness moves to whoever emits it now
(tan); nothing in this repo can stand in for that check, and treating a
green run here as emitter conformance would be exactly the vacuous-green
shape this migration is trying to avoid.

With --manifest PATH it validates an existing manifest (e.g. a real
build/system-manifest.yaml an IDE consumes), unchanged.

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
CORPUS_DIR = REPO / "tests" / "fixtures" / "emit-snapshots"
CORPUS_GLOB = "*.system-manifest.snap"


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


def _default_corpus() -> list[Path]:
    """The committed system-manifest snapshots -- see the module docstring's
    WHAT WAS LOST for why this replaced regenerating from board.yaml."""
    return sorted(CORPUS_DIR.glob(CORPUS_GLOB))


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Validate system-manifest.yaml against the v1 contract.")
    ap.add_argument("--manifest", type=Path, action="append", default=[],
                    help="manifest file(s) to validate. Default: validate the "
                         "committed corpus at tests/fixtures/emit-snapshots/"
                         "*.system-manifest.snap")
    ap.add_argument("--schema", type=Path, default=SCHEMA)
    args = ap.parse_args()

    validator = _make_validator(args.schema)
    if args.manifest:
        targets = args.manifest
    else:
        targets = _default_corpus()
        if not targets:
            print(f"FAIL corpus empty: {CORPUS_DIR / CORPUS_GLOB} matched no "
                  f"files -- a gate whose corpus silently becomes zero files "
                  f"is passing vacuously, not validating anything")
            return 1

    failures = sum(_validate_file(p, validator) for p in targets)
    print(f"\n{len(targets)} manifest(s) checked, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
