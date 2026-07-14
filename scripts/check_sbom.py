#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""CI gate: gen_sbom.py produces a valid, deterministic CycloneDX SBOM (#610 §7).

Generates the SBOM from the real alp.lock and asserts CycloneDX shape
(bomFormat/specVersion/components present, every component has a name) plus
determinism (build_sbom(lock) called twice yields the identical bom -- no
wall-clock, no randomness). stdlib only.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "scripts"))

import gen_sbom  # noqa: E402

LOCK = ROOT / "alp.lock"


def find_problems(lock_path: Path) -> list[str]:
    problems: list[str] = []
    if not lock_path.is_file():
        return [f"missing {lock_path}"]
    lock = json.loads(lock_path.read_text(encoding="utf-8"))
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
    problems = find_problems(LOCK)
    if problems:
        for p in problems:
            print(f"sbom: {p}", file=sys.stderr)
        return 1
    print("OK: gen_sbom.py produces a valid, deterministic CycloneDX SBOM.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
