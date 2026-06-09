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
sys.path.insert(0, str(Path(__file__).resolve().parent))
DEFAULT_PUBKEY = REPO / "keys" / "alp_release_signing_ecdsa_p256.pub.pem"


def _validate(path: Path, validator: jsonschema.Draft202012Validator, pubkey_path=None, require_signature=False) -> int:
    rel = path.name
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"FAIL {rel}: parse error ({e})")
        return 1
    errors = sorted(validator.iter_errors(doc), key=lambda e: list(e.absolute_path))
    if errors:
        print(f"FAIL {rel}")
        for err in errors:
            loc = "/".join(str(p) for p in err.absolute_path) or "<root>"
            print(f"  · {loc}: {err.message}")
        return 1
    sig = doc.get("signature")
    if sig:
        import som_signing  # lazy: only the verify path needs cryptography
        pub_path = pubkey_path or DEFAULT_PUBKEY
        if not pub_path.exists():
            print(f"FAIL {rel}: signature present but no public key at {pub_path}")
            return 1
        pub = som_signing.load_public_key_file(pub_path)
        if not som_signing.verify_signature(doc, pub):
            print(f"FAIL {rel}: signature verification failed")
            return 1
        print(f"OK   {rel}  (release_version={doc.get('release_version', '?')}, "
              f"status={doc.get('status', '?')}, signature verified key_id={sig.get('key_id')})")
        return 0
    if require_signature:
        print(f"FAIL {rel}: unsigned but --require-signature set")
        return 1
    print(f"OK   {rel}  (release_version={doc.get('release_version', '?')}, status={doc.get('status', '?')})")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="Validate SoM-release bundle manifests.")
    ap.add_argument("--bundle", type=Path, action="append", default=[],
                    help="bundle.json to validate (repeatable). Default: the shipped example.")
    ap.add_argument("--schema", type=Path, default=SCHEMA)
    ap.add_argument("--pubkey", type=Path, default=None,
                    help="public key PEM to verify signatures against "
                         "(default: keys/alp_release_signing_ecdsa_p256.pub.pem)")
    ap.add_argument("--require-signature", action="store_true",
                    help="fail if a bundle is unsigned")
    args = ap.parse_args()

    schema = json.loads(args.schema.read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator.check_schema(schema)
    validator = jsonschema.Draft202012Validator(
        schema, format_checker=jsonschema.FormatChecker())

    targets = args.bundle or [EXAMPLE]
    failures = sum(_validate(p, validator, args.pubkey, args.require_signature) for p in targets)
    print(f"\n{len(targets)} bundle(s) checked, {failures} failure(s)")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
