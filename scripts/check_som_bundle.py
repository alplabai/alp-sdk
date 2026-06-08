#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Validate SoM-release bundle manifests against the public
metadata/schemas/som-release-bundle-v1.schema.json contract.

With no --bundle argument, validates the shipped public example at
metadata/templates/som-release-bundle.example.json (the CI gate). With
one or more --bundle PATH, validates those (used by the Piece-4
provisioning tool + by operators against a real bundle.json).

Run locally:

    python3 scripts/check_som_bundle.py
    python3 scripts/check_som_bundle.py --bundle path/to/bundle.json
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import jsonschema

REPO = Path(__file__).resolve().parent.parent
SCHEMA = REPO / "metadata" / "schemas" / "som-release-bundle-v1.schema.json"
EXAMPLE = REPO / "metadata" / "templates" / "som-release-bundle.example.json"


def _validate(path: Path, validator: jsonschema.Draft202012Validator) -> int:
    rel = path.name
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"FAIL {rel}: parse error ({e})")
        return 1
    errors = sorted(validator.iter_errors(doc), key=lambda e: list(e.absolute_path))
    # Augment schema errors with a semantic note when status=complete is missing
    # a system_image component (the if/then schema emits a generic "contains"
    # message; make the role name explicit so callers can parse it).
    extra = []
    if doc.get("status") == "complete":
        roles = [c.get("role") for c in doc.get("components", []) if isinstance(c, dict)]
        if "system_image" not in roles:
            extra.append("components: status is 'complete' but no system_image component found")
    if errors or extra:
        print(f"FAIL {rel}")
        for err in errors:
            loc = "/".join(str(p) for p in err.absolute_path) or "<root>"
            print(f"  - {loc}: {err.message}")
        for msg in extra:
            print(f"  - {msg}")
        return 1
    print(f"OK   {rel}  (release_version={doc.get('release_version', '?')}, status={doc.get('status', '?')})")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Validate SoM-release bundle manifests.")
    ap.add_argument("--bundle", type=Path, action="append", default=[],
                    help="bundle.json to validate (repeatable). Default: the shipped example.")
    ap.add_argument("--schema", type=Path, default=SCHEMA)
    args = ap.parse_args()

    schema = json.loads(args.schema.read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator.check_schema(schema)
    validator = jsonschema.Draft202012Validator(schema)

    targets = args.bundle or [EXAMPLE]
    failures = sum(_validate(p, validator) for p in targets)
    print(f"\n{len(targets)} bundle(s) checked, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
