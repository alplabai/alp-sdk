#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Validate the versioned machine diagnostics contract
(metadata/schemas/diagnostic-v1.schema.json) two ways:

  1. Schema self-validity (Draft 2020-12).
  2. A REAL document, produced by actually running `alp validate --format
     json` against a known-bad board.yaml fixture (never a hand-written
     sample), validates against the schema. This is the same
     drift-can't-happen posture as scripts/check_emit_registry.py: the
     gate exercises the real emitter, not a copy of its output.

Run locally:

    python3 scripts/check_diagnostic_schema.py
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

import jsonschema

REPO = Path(__file__).resolve().parent.parent
SCHEMA = REPO / "metadata" / "schemas" / "diagnostic-v1.schema.json"
FIXTURE = REPO / "tests" / "fixtures" / "board_yaml_bad" / "ALP-B005-bad-sku.yaml"


def _make_validator(schema_path: Path) -> jsonschema.Draft202012Validator:
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator.check_schema(schema)
    return jsonschema.Draft202012Validator(schema)


def _run_alp_validate(fixture: Path) -> dict:
    env = dict(os.environ)
    env["PYTHONPATH"] = str(REPO / "scripts")
    proc = subprocess.run(
        [sys.executable, "-m", "alp_cli.main", "validate", str(fixture),
         "--format", "json"],
        cwd=REPO, env=env, capture_output=True, text=True,
    )
    try:
        return json.loads(proc.stdout)
    except json.JSONDecodeError as e:
        raise SystemExit(
            f"check_diagnostic_schema: `alp validate --format json` did not "
            f"print a JSON document to stdout ({e}).\nstdout: {proc.stdout!r}"
            f"\nstderr: {proc.stderr!r}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--schema", type=Path, default=SCHEMA)
    ap.add_argument("--fixture", type=Path, default=FIXTURE)
    args = ap.parse_args()

    validator = _make_validator(args.schema)
    doc = _run_alp_validate(args.fixture)

    errors = sorted(validator.iter_errors(doc), key=lambda e: list(e.absolute_path))
    if errors:
        print(f"FAIL {args.schema.relative_to(REPO).as_posix()}")
        for err in errors:
            loc = "/".join(str(p) for p in err.absolute_path) or "<root>"
            print(f"  · {loc}: {err.message}")
        return 1

    if not doc.get("diagnostics"):
        print("check_diagnostic_schema: fixture produced zero diagnostics -- "
              "the gate needs at least one to exercise the schema.")
        return 1

    print(f"OK   {args.schema.relative_to(REPO).as_posix()}  "
          f"(self-valid, and a real `alp validate --format json` document "
          f"conforms, {len(doc['diagnostics'])} diagnostic(s))")
    return 0


if __name__ == "__main__":
    sys.exit(main())
