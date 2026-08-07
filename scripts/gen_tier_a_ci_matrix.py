#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Derive the ADR 0018 Tier-A library CI build matrix from the single source
of truth, metadata/registries/tier-a-library-ci.json's `familyMatrix`.

Fixes #499: .github/workflows/pr-tier-a-libraries.yml used to hard-code
its `strategy.matrix.include` som/core pairs inline, so a `familyMatrix`
edit in the registry silently had no effect on the actual CI matrix. This
script is the one place that turns `familyMatrix` into the GitHub Actions
matrix JSON; the workflow's `compute-family-matrix` job runs it and feeds
the result to the build job via `needs.compute-family-matrix.outputs.matrix`
/ `fromJson(...)`, so the two can no longer diverge.

Output is a GitHub Actions matrix object, `{"include": [{"som": ...,
"core": ...}, ...]}`, one entry per `familyMatrix` cell EXCEPT a cell
whose `family` is a key in `excludedFamilies` (a documented,
build-time-only skip -- e.g. #1025's NX9101/tbd hw_rev -- the cell
itself stays in `familyMatrix` so `scripts/validate_metadata.py` still
sees every SoM family represented), on a single line of stdout (safe
to assign straight to `$GITHUB_OUTPUT`).

Usage:

    python3 scripts/gen_tier_a_ci_matrix.py
    python3 scripts/gen_tier_a_ci_matrix.py --registry path/to/other.json
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_REGISTRY = REPO / "metadata" / "registries" / "tier-a-library-ci.json"


def build_matrix(registry_path: Path) -> dict:
    registry = json.loads(registry_path.read_text(encoding="utf-8"))
    family_matrix = registry.get("familyMatrix")
    if not isinstance(family_matrix, list) or not family_matrix:
        raise ValueError(f"{registry_path}: familyMatrix is missing or empty")

    # familyMatrix cells named here stay in the registry (so
    # scripts/validate_metadata.py still sees every SoM family covered)
    # but are skipped from the actual CI build matrix -- e.g. a family
    # whose only SoM can't emit at all right now (#1025: E1M-NX9101's
    # hw_rev is `status: tbd`). Same shape as hostBuild.excludedLibraries.
    excluded_families = registry.get("excludedFamilies") or {}

    include = []
    for idx, cell in enumerate(family_matrix):
        if not isinstance(cell, dict):
            raise ValueError(f"{registry_path}: familyMatrix[{idx}] is not an object")
        family = cell.get("family")
        som = cell.get("som")
        core = cell.get("core")
        if not isinstance(som, str) or not som:
            raise ValueError(f"{registry_path}: familyMatrix[{idx}]/som is missing")
        if not isinstance(core, str) or not core:
            raise ValueError(f"{registry_path}: familyMatrix[{idx}]/core is missing")
        if isinstance(family, str) and family in excluded_families:
            continue
        include.append({"som": som, "core": core})

    if not include:
        raise ValueError(f"{registry_path}: every familyMatrix cell is excluded -- "
                          f"nothing left to build")
    return {"include": include}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--registry",
        type=Path,
        default=DEFAULT_REGISTRY,
        help="Path to tier-a-library-ci.json (default: %(default)s)",
    )
    args = parser.parse_args()

    try:
        matrix = build_matrix(args.registry)
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"gen_tier_a_ci_matrix: {exc}", file=sys.stderr)
        return 1

    print(json.dumps(matrix, separators=(",", ":")))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
