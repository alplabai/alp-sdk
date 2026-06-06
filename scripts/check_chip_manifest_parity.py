#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Chip driver <-> manifest parity gate -- fails (exit 1) when a chip driver
ships without its machine-readable metadata manifest, or a manifest ships
with no driver, unless the mismatch is on an explicit allowlist.

Correspondence rule (single-source-of-truth convention):
    chips/<id>/<id>.c   <-->   metadata/chips/<id>.yaml

This is the root-cause gate for the gap-review finding "9 drivers lack a
manifest and pca9451a is a manifest with no driver": new drift fails CI
immediately. The allowlists are a RATCHET -- the gate also fails if an
allowlisted entry is stale (a driver in KNOWN_DRIVER_NO_MANIFEST that now
HAS a manifest, or vice-versa), so the backlog can only shrink, never grow
silently.
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
CHIPS_DIR = REPO / "chips"
MANIFEST_DIR = REPO / "metadata" / "chips"

# Drivers known to lack a manifest today. BACKLOG ONLY -- shrink this list by
# authoring metadata/chips/<id>.yaml; do not add new entries (a new driver
# without a manifest must fail the gate, not be allowlisted away).
KNOWN_DRIVER_NO_MANIFEST = {
    "bmi323",
    "bmp581",
    "cam_mux_pi3wvr626",
    "lis2dw12",
    "ssd1331",
    "tcal9538",
}

# Manifests intentionally without a driver (planned-but-unimplemented parts).
KNOWN_MANIFEST_NO_DRIVER = {
    "pca9451a",  # PMIC: metadata declared; driver intentionally not yet built.
}


def _drivers() -> set[str]:
    return {
        d.name
        for d in CHIPS_DIR.iterdir()
        if d.is_dir() and (d / f"{d.name}.c").is_file()
    }


def _manifests() -> set[str]:
    return {f.stem for f in MANIFEST_DIR.glob("*.yaml")}


def main() -> int:
    drivers = _drivers()
    manifests = _manifests()

    driver_no_manifest = drivers - manifests
    manifest_no_driver = manifests - drivers

    errors: list[str] = []

    # New, un-allowlisted drift -> hard fail.
    for cid in sorted(driver_no_manifest - KNOWN_DRIVER_NO_MANIFEST):
        errors.append(
            f"driver chips/{cid}/{cid}.c has no metadata/chips/{cid}.yaml manifest"
        )
    for cid in sorted(manifest_no_driver - KNOWN_MANIFEST_NO_DRIVER):
        errors.append(
            f"manifest metadata/chips/{cid}.yaml has no chips/{cid}/ driver"
        )

    # Stale allowlist entries -> hard fail (forces the backlog to shrink).
    for cid in sorted(KNOWN_DRIVER_NO_MANIFEST - driver_no_manifest):
        errors.append(
            f"KNOWN_DRIVER_NO_MANIFEST lists '{cid}' but it is no longer a gap "
            f"-- remove it from the allowlist in {Path(__file__).name}"
        )
    for cid in sorted(KNOWN_MANIFEST_NO_DRIVER - manifest_no_driver):
        errors.append(
            f"KNOWN_MANIFEST_NO_DRIVER lists '{cid}' but it now has a driver "
            f"-- remove it from the allowlist in {Path(__file__).name}"
        )

    backlog = sorted(driver_no_manifest & KNOWN_DRIVER_NO_MANIFEST)
    if backlog:
        print(
            f"note: {len(backlog)} driver(s) still awaiting a manifest "
            f"(tracked backlog): {', '.join(backlog)}"
        )

    if errors:
        print("chip driver<->manifest parity check FAILED:", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    print(
        f"chip parity OK: {len(drivers)} drivers, {len(manifests)} manifests "
        f"({len(backlog)} backlog, {len(KNOWN_MANIFEST_NO_DRIVER)} manifest-only)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
