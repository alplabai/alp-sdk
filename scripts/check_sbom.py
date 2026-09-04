#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""CI gate: gen_sbom.py produces a valid, deterministic CycloneDX SBOM (#610 §7).

alp.lock is generated on demand, not committed (#1576), so this gate builds
one in memory (same call `west alp-lock` makes) and asserts CycloneDX shape
(bomFormat/specVersion/components present, every component has a name) plus
determinism (build_sbom(lock) called twice yields the identical bom -- no
wall-clock, no randomness).
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))

import alp_lock  # noqa: E402
import gen_sbom  # noqa: E402


def find_problems(lock: dict) -> list[str]:
    problems: list[str] = []
    a = gen_sbom.build_sbom(lock)
    b = gen_sbom.build_sbom(lock)
    if a.get("bomFormat") != "CycloneDX":
        problems.append("bomFormat must be CycloneDX")
    if a.get("specVersion") != "1.5":
        problems.append("specVersion must be 1.5")
    if not a.get("components"):
        problems.append("components must be non-empty")
    for c in a.get("components", []):
        if not c.get("name"):
            problems.append(f"component missing name: {c}")
    if gen_sbom.digest_json(a) != gen_sbom.digest_json(b):
        problems.append("build_sbom is not deterministic: two calls differ")
    if a.get("serialNumber") != b.get("serialNumber"):
        problems.append("serialNumber is not deterministic")
    return problems


def main() -> int:
    try:
        lock = alp_lock.build_lock(ROOT)
    except alp_lock.LockError as e:
        print(f"sbom: {e}", file=sys.stderr)
        return 1
    problems = find_problems(lock)
    if problems:
        for p in problems:
            print(f"sbom: {p}", file=sys.stderr)
        return 1
    print("OK: gen_sbom.py produces a valid, deterministic CycloneDX SBOM.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
