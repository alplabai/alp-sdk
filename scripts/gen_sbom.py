#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Deterministic CycloneDX 1.5 SBOM from alp.lock (#610 §7).

Composes the SDK + its west projects + curated libraries into a CycloneDX bom.
No wall-clock: serialNumber is derived from the lock's own digest, so identical
alp.lock -> byte-identical SBOM (feeds the reproducible build receipt).

    python3 scripts/gen_sbom.py [--lock alp.lock] [--output sbom.cdx.json]
"""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def digest_json(obj: dict) -> str:
    return "sha256:" + hashlib.sha256(
        json.dumps(obj, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def _component(name, version, ctype="library", license_id=None):
    c = {"type": ctype, "name": name}
    if version:
        c["version"] = str(version)
    if license_id:
        c["licenses"] = [{"license": {"id": license_id}}]
    return c


def build_sbom(lock: dict) -> dict:
    sdk = lock.get("sdk", {})
    comps = [_component("alp-sdk", sdk.get("version"), "application")]
    for p in lock.get("west", {}).get("projects", []):
        comps.append(_component(p["name"], p.get("revision")))
    for lib in lock.get("libraries", []):
        comps.append(_component(lib["name"], lib.get("version"),
                                license_id=lib.get("license")))
    comps.sort(key=lambda c: (c["type"], c["name"]))
    # stable serial number from the lock content (no randomness / clock)
    serial = "urn:uuid:" + hashlib.sha256(
        json.dumps(lock, sort_keys=True).encode()).hexdigest()[:32]
    return {
        "bomFormat": "CycloneDX",
        "specVersion": "1.5",
        "serialNumber": serial,
        "version": 1,
        "metadata": {"component": comps[0]},
        "components": comps,
    }


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="generate a CycloneDX SBOM from alp.lock")
    ap.add_argument("--lock", default=str(ROOT / "alp.lock"))
    ap.add_argument("--output")
    args = ap.parse_args(argv)
    lock = json.loads(Path(args.lock).read_text(encoding="utf-8"))
    sbom = build_sbom(lock)
    text = json.dumps(sbom, indent=2) + "\n"
    if args.output:
        Path(args.output).write_text(text, encoding="utf-8")
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
