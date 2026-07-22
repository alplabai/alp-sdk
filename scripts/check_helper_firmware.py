#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Helper-MCU firmware checksum gate (issue #852, metadata half).

Every `helper_firmware[]` entry in `metadata/e1m_modules/E1M-*.yaml` (the
GD32G553 supervisor bridge, the CC3501E Wi-Fi/BLE bridge) carries a `sha256:`
alongside its flat `firmware_path`.  This gate walks every entry and:

  - when the referenced artefact is PRESENT on disk, verifies its SHA-256
    matches the metadata -- a mismatch is a hard failure (a stale or
    tampered binary would otherwise ship silently);
  - when the artefact is ABSENT, reports it clearly and NON-FATALLY.  The
    GD32 bridge's `firmware_path` (`firmware/gd32-bridge/build/gd32/
    gd32-bridge.bin`) is a gitignored developer build output that does not
    exist on a fresh clone or in CI -- failing the gate on that would red
    every fresh-clone run, so `absent` is reported, not raised.

This gate does NOT build firmware, select a per-SoM/variant image, or decide
whether GD32/CC3501E become a real build slice -- that is the larger fork
issue #852 leaves open for the external tan-cli executor.

Run locally:

    python3 scripts/check_helper_firmware.py
"""

from __future__ import annotations

import argparse
import hashlib
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
E1M_MODULES = REPO / "metadata" / "e1m_modules"

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("check_helper_firmware: PyYAML is required.")


def _sha256(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def check_entry(sku: str, entry: dict, root: Path) -> tuple[str, list[str]]:
    """Return (status, errors) for one helper_firmware entry.

    status is one of: verified, mismatch, absent, unchecked, not-applicable.
    Only `mismatch` populates `errors` (the hard-failing case).
    """
    name = entry.get("name", "<unnamed>")
    firmware_path = entry.get("firmware_path")
    sha256 = entry.get("sha256")

    if not firmware_path or firmware_path == "TBD":
        return "not-applicable", []

    path = root / firmware_path
    if not path.is_file():
        print(f"ABSENT   {sku}/{name}: {firmware_path} not found on disk "
              f"(gitignored build output or not yet fetched) -- non-fatal")
        return "absent", []

    if not sha256 or sha256 == "TBD":
        print(f"UNCHECKED {sku}/{name}: {firmware_path} present but sha256 "
              f"is TBD in metadata -- nothing to verify against")
        return "unchecked", []

    actual = _sha256(path)
    if actual != sha256:
        return "mismatch", [
            f"{sku}/{name}: {firmware_path} sha256 mismatch -- "
            f"metadata says {sha256}, actual is {actual}"
        ]

    print(f"VERIFIED {sku}/{name}: {firmware_path} sha256 matches metadata")
    return "verified", []


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args(argv)

    errors: list[str] = []
    counts: dict[str, int] = {}

    for path in sorted((args.root / "metadata" / "e1m_modules").glob("E1M-*.yaml")):
        doc = yaml.safe_load(path.read_text(encoding="utf-8")) or {}
        helper_firmware = doc.get("helper_firmware")
        if not isinstance(helper_firmware, list):
            continue
        for entry in helper_firmware:
            if not isinstance(entry, dict):
                continue
            status, entry_errors = check_entry(path.stem, entry, args.root)
            counts[status] = counts.get(status, 0) + 1
            errors.extend(entry_errors)

    print(f"\ncheck_helper_firmware: {dict(counts)}")

    if errors:
        for e in errors:
            print(f"check_helper_firmware: {e}", file=sys.stderr)
        print(f"check_helper_firmware: {len(errors)} issue(s).", file=sys.stderr)
        return 1
    print("check_helper_firmware: no checksum mismatches.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
