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


def _bmap_problem(bundle_dir: Path, doc: dict) -> str | None:
    """system_image_bmap must be a valid bmap whose ImageSize matches the gunzipped
    system_image. Checked only when both files sit beside the bundle (a manifest
    alone, like the shipped example, has nothing to open)."""
    files = {c["role"]: bundle_dir / c["file"] for c in doc["components"]}
    bm_path, img = files.get("system_image_bmap"), files.get("system_image")
    if bm_path is None or not bm_path.is_file():
        return None
    from provision import bmap  # lazy: only bundles carrying a bmap need it
    try:
        bm = bmap.parse(bm_path)
    except Exception as e:
        return str(e)
    if img is not None and img.is_file() and img.suffix == ".gz":
        with open(img, "rb") as f:         # gzip ISIZE: uncompressed size mod 2**32 (cheap, no gunzip)
            f.seek(-4, 2)
            isize = int.from_bytes(f.read(4), "little")
        if isize != bm.image_size % (1 << 32):
            return f"ImageSize {bm.image_size} does not match the gunzipped {img.name} (ISIZE {isize})"
    return None


def _cm33_problem(bundle_dir: Path, doc: dict) -> str | None:
    """cm33 is the stored (padded) image BL2 loads raw; run the flash runner's sanity checks on it.
    Checked only when the file sits beside the bundle."""
    c = next((c for c in doc["components"] if c["role"] == "cm33"), None)
    if c is None or not (bundle_dir / c["file"]).is_file():
        return None
    from provision import gates  # lazy: only bundles carrying a cm33 image need it
    return "; ".join(gates.cm33_problems((bundle_dir / c["file"]).read_bytes())) or None


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
    # JSON Schema cannot say "unique by role"; consumers look components up by role.
    roles = [c["role"] for c in doc["components"]]
    dupes = sorted({r for r in roles if roles.count(r) > 1})
    if dupes:
        print(f"FAIL {rel}")
        print(f"  · components: duplicate role(s) {dupes}")
        return 1
    if (why := _bmap_problem(path.parent, doc)):
        print(f"FAIL {rel}")
        print(f"  · system_image_bmap: {why}")
        return 1
    if (why := _cm33_problem(path.parent, doc)):
        print(f"FAIL {rel}")
        print(f"  · cm33: {why}")
        return 1
    sig = doc.get("signature")
    if sig:
        import som_signing  # lazy: only the verify path needs cryptography
        pub_path = pubkey_path or DEFAULT_PUBKEY
        if not pub_path.exists():
            print(f"FAIL {rel}: signature present but no public key at {pub_path}")
            return 1
        try:
            pub = som_signing.load_public_key_file(pub_path)
        except Exception as e:
            print(f"FAIL {rel}: cannot load public key {pub_path}: {e}")
            return 1
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
