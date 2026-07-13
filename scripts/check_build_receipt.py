#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Validate the build-receipt-v1 schema is well-formed (#610 §7).

Receipts are per-build artifacts, so there's no repo-wide receipt to
drift-check; this gate asserts the schema itself is a valid, closed
Draft-2020-12 schema (a broken schema would silently pass every build's
receipt validation). stdlib + jsonschema.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

import jsonschema

SCHEMA = Path(__file__).resolve().parent.parent / "metadata/schemas/build-receipt-v1.schema.json"


def main() -> int:
    s = json.loads(SCHEMA.read_text(encoding="utf-8"))
    try:
        jsonschema.Draft202012Validator.check_schema(s)
    except jsonschema.SchemaError as e:
        print(f"build-receipt schema invalid: {e.message}", file=sys.stderr)
        return 1
    if s.get("additionalProperties") is not False or s["properties"]["schemaVersion"]["const"] != 1:
        print("build-receipt schema must be closed with schemaVersion const 1", file=sys.stderr)
        return 1
    print("OK: build-receipt-v1 schema is valid.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
