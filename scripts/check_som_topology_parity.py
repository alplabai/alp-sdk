#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: every `id:` in every `metadata/socs/**/*.json` `cores[]` block
must have a matching key in every consuming SoM preset's `topology:`
block. Catches the case where a silicon update adds a new core (e.g.
m33_sm) but the SoM preset's topology stops declaring an OS for it. Per
design doc section 4.2 the topology key set is required to mirror the
SoC's `cores[].id` set exactly.

A SoM preset's `silicon:` key (colon-separated `vendor:family:part`, e.g.
`alif:ensemble:e8`) that does not resolve to a real `metadata/socs/`
spec is ALSO a failure -- not a skip -- so topology drift can't hide
behind a typo'd or stale `silicon:` value.

Extracted from an inline YAML step in `pr-metadata-validate.yml` (issue
#997): the inline copy hand-rolled the `silicon:` -> `metadata/socs/`
path resolution a THIRD time (alongside `_resolve_silicon_variant()` and
`resolve_capabilities()` in `alp_project_loader.py`), untested and
un-runnable locally -- exactly how the `soc_ref`/`soc` bug in #995 rotted
unnoticed.

This gate no longer shares `alp_project_loader.resolve_soc_path()`:
`alp_project_loader.py` is doomed code slated for deletion in a later
slice of the tan/sdk gate migration, so this gate inlines the same
`silicon:` -> `metadata/socs/<vendor>/<family>/<part>.json` resolution
(`_resolve_soc_path()` below, copied verbatim from that helper's logic)
instead of importing it. Nothing is lost: the resolution is one `Path`
join plus a 3-part guard, reproduced here so this gate stays pure
metadata -- no import of any doomed module, no subprocess -- and keeps
every tooth described above.

Run locally:

    python scripts/check_som_topology_parity.py

CI wires this in `pr-metadata-validate.yml`.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent


def _resolve_soc_path(silicon: str | None, metadata_root: Path) -> Path | None:
    """Resolve a `vendor:family:part` `silicon:` key to the SoC-JSON path
    it names: `metadata/socs/<vendor>/<family>/<part>.json`.

    Returns None when `silicon` is falsy or not exactly 3 colon-separated
    parts -- does NOT check the path exists, the caller decides what an
    unresolved/missing SoC spec means (here: a hard failure, never a
    skip -- see module docstring).

    Inlined from `alp_project_loader.resolve_soc_path()` so this gate
    does not import that (doomed) module; the resolution itself is
    unchanged.
    """
    if not silicon:
        return None
    parts = silicon.split(":")
    if len(parts) != 3:
        return None
    return metadata_root / "socs" / parts[0] / parts[1] / f"{parts[2]}.json"


def _soc_cores_by_path(socs_dir: Path) -> dict[str, set[str]]:
    """Index every SoC JSON's `cores[].id` set, keyed by the file's
    resolved path (as a string, so it compares equal to a
    `_resolve_soc_path()` result regardless of glob ordering)."""
    cores: dict[str, set[str]] = {}
    if not socs_dir.is_dir():
        return cores
    for jf in socs_dir.rglob("*.json"):
        with jf.open(encoding="utf-8") as f:
            data = json.load(f)
        ids = [c.get("id") for c in data.get("cores", []) if c.get("id")]
        cores[str(jf.resolve())] = set(ids)
    return cores


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    presets_dir = root / "metadata" / "e1m_modules"
    if not presets_dir.is_dir():
        return problems

    soc_cores = _soc_cores_by_path(root / "metadata" / "socs")

    for preset in sorted(presets_dir.glob("*.yaml")):
        with preset.open(encoding="utf-8") as f:
            doc = yaml.safe_load(f) or {}
        topology = doc.get("topology") or {}
        if not topology:
            continue

        silicon = doc.get("silicon") or ""
        soc_path = _resolve_soc_path(silicon, root / "metadata")
        key = str(soc_path.resolve()) if soc_path is not None and soc_path.is_file() else None
        if key is None or key not in soc_cores:
            problems.append(
                f"{preset.name}: silicon={silicon!r} does not resolve to a "
                "metadata/socs/ spec; topology cannot be cross-checked"
            )
            continue

        expected = soc_cores[key]
        actual = set(topology.keys())
        if expected != actual:
            problems.append(
                f"topology mismatch in {preset.name}: SoC "
                f"{soc_path.relative_to(root)} declares "
                f"cores={sorted(expected)}, topology has {sorted(actual)}"
            )

    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    problems = find_problems(args.root)
    if problems:
        print("check_som_topology_parity: found problems:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1
    print("OK: every SoM preset's topology: keys match its resolved SoC's cores[].id set.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
