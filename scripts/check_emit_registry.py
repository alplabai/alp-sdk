#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Validate metadata/emit-registry-v1.json against the public IDE/tool contract
(metadata/schemas/emit-registry-v1.schema.json) and against the emit evidence
this repository still holds.

THE REGISTRY IS NOW A CATALOGUE, NOT A CONTRACT AGAINST THIS REPO'S CODE.

That is a real demotion and it is worth stating rather than discovering. This
gate used to `ast`-parse the `--emit ... choices=[...]` list out of
`scripts/alp_project.py` and `scripts/alp_orchestrate/cli.py` and demand exact
equality with the registry, in both directions. Both of those files are being
deleted -- the planner they front now lives in the tan repository -- and after
that no file in alp-sdk implements or enumerates the full emit surface. The
question "does the code expose exactly these modes?" stops being answerable
here, so the registry stops answering it: it becomes the SOURCE OF TRUTH for
the mode set, published for IDE/tool consumers, and this gate keeps it honest
against what alp-sdk can still see.

What alp-sdk can still see are two PARTIAL enumerations, and both are checked
as subsets rather than equalities, because neither covers the whole surface:

  * `tests/fixtures/emit-snapshots/*.snap` -- the committed byte-parity
    snapshots. A snapshot is proof a mode exists and is exercised, so a mode
    with a snapshot and no registry entry is a hole in the catalogue.
  * `scripts/west_commands/alp_emit.py`'s `_EMIT_MODES` -- the user-facing
    `west alp-emit` list, 8 of the 20 modes. Read by `ast`, never hardcoded
    here, so this half cannot itself drift.

WHAT WAS LOST, deliberately: the PHANTOM direction. A registry entry that
nothing anywhere implements can no longer be caught from this repository --
the implementation is in another one. That check has to be re-established
tan-side (against `tan generate`'s targets plus the planner CLI's modes) if it
is wanted; nothing here can stand in for it, and pretending otherwise by
checking the registry against itself would be the vacuous-green shape this
migration is trying to avoid.

`owner.module` is still checked, but only where it names a path INSIDE this
repo: a catalogue that describes modules which no longer exist is fiction, and
the deletion slice should repoint those fields at the executor in the same
commit that removes the modules (a one-line edit per mode). Fields naming
anything outside this repo are informational and not verified here -- alp-sdk
cannot see another repo's tree, and a gate that pretends to is worse than one
that admits the boundary.

Run locally:

    python3 scripts/check_emit_registry.py
"""

from __future__ import annotations

import argparse
import ast
import json
import sys
from pathlib import Path

import jsonschema

REPO = Path(__file__).resolve().parent.parent
REGISTRY = REPO / "metadata" / "emit-registry-v1.json"
SCHEMA = REPO / "metadata" / "schemas" / "emit-registry-v1.schema.json"
SNAPSHOT_DIR = REPO / "tests" / "fixtures" / "emit-snapshots"
WEST_ALP_EMIT = REPO / "scripts" / "west_commands" / "alp_emit.py"


def snapshot_fields(snapshot_dir: Path = SNAPSHOT_DIR) -> dict[str, set[str]]:
    """`{filename: {dot-separated fields}}` for every committed snapshot.

    Derived from the FILENAMES, not from `check_emit_snapshots.py`'s `CASES`
    table: that table is Python that imports the planner to run, and this gate
    has to keep working after the planner is gone.

    Fields, not "the mode", because the fixtures use two conventions and both
    are load-bearing: `proj-aen.zephyr-conf.snap` puts the mode last, while the
    template family (`scaffold.minimal-v2n101.snap`, 4 files) puts it FIRST.
    Guessing a position would have failed on four real files -- it did, before
    this was written this way -- so the caller asks the honest question instead:
    does any field of this name match a catalogued mode?
    """
    return {snap.name: set(snap.name[: -len(".snap")].split("."))
            for snap in sorted(snapshot_dir.glob("*.snap"))}


def west_emit_modes(source: Path = WEST_ALP_EMIT) -> set[str]:
    """`_EMIT_MODES` from `west alp-emit`, via ast so it cannot drift from the
    list a user actually gets in their shell."""
    tree = ast.parse(source.read_text(encoding="utf-8"))
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign):
            continue
        names = [t.id for t in node.targets if isinstance(t, ast.Name)]
        if "_EMIT_MODES" not in names:
            continue
        if isinstance(node.value, (ast.List, ast.Tuple)):
            return {e.value for e in node.value.elts if isinstance(e, ast.Constant)}
    raise SystemExit(
        f"check_emit_registry: could not find `_EMIT_MODES = [...]` in "
        f"{source.relative_to(REPO).as_posix()} -- if `west alp-emit` stopped "
        f"declaring its modes there, point this gate at wherever it does, "
        f"rather than dropping the check.")


def _make_validator(schema_path: Path) -> jsonschema.Draft202012Validator:
    schema = json.loads(schema_path.read_text(encoding="utf-8"))
    jsonschema.Draft202012Validator.check_schema(schema)
    return jsonschema.Draft202012Validator(
        schema, format_checker=jsonschema.FormatChecker())


def _validate_schema(doc: dict, validator: jsonschema.Draft202012Validator) -> list[str]:
    errors = sorted(validator.iter_errors(doc), key=lambda e: list(e.absolute_path))
    out = []
    for err in errors:
        loc = "/".join(str(p) for p in err.absolute_path) or "<root>"
        out.append(f"{loc}: {err.message}")
    return out


def _check_evidence(doc: dict) -> tuple[list[str], int]:
    """Every mode alp-sdk still has evidence for must be catalogued.

    Returns (problems, count of catalogued modes with no in-repo evidence) --
    the second value is printed on success so the erosion is visible: if it
    ever reaches the full mode count, this gate is checking nothing and should
    be replaced rather than trusted.
    """
    registry_modes = [m["mode"] for m in doc.get("modes", [])]
    catalogued = set(registry_modes)
    problems = []

    duplicates = sorted({m for m in registry_modes if registry_modes.count(m) > 1})
    if duplicates:
        problems.append(f"duplicate mode entries in the registry: {duplicates}")

    snapshots = snapshot_fields()
    orphans = sorted(name for name, fields in snapshots.items()
                     if not (fields & catalogued))
    if orphans:
        problems.append(
            "snapshots under tests/fixtures/emit-snapshots/ whose filename "
            f"names no catalogued mode: {orphans} -- a fixture exists for an "
            f"emit the registry does not describe")

    west_drift = sorted(west_emit_modes() - catalogued)
    if west_drift:
        problems.append(
            "modes `west alp-emit` offers (scripts/west_commands/alp_emit.py "
            f"_EMIT_MODES) but the registry does not list: {west_drift}")

    evidenced = {m for m in catalogued
                 if any(m in fields for fields in snapshots.values())}
    evidenced |= west_emit_modes() & catalogued
    return problems, len(catalogued - evidenced)


def _check_owner_paths(doc: dict) -> list[str]:
    """An `owner.module` naming a path in THIS repo must exist.

    Not cosmetic: these fields are what a reader follows to find the
    implementation, and the planner modules they currently name are being
    deleted. When that happens this check fails until the fields are repointed
    at the executor -- which is the intended coupling, and one JSON edit per
    mode. Paths that do not look like this repo's are left alone; alp-sdk
    cannot verify another repository's tree.
    """
    problems = []
    for entry in doc.get("modes", []):
        module = (entry.get("owner") or {}).get("module")
        if not module or not module.startswith("scripts/"):
            continue
        if not (REPO / module).exists():
            problems.append(
                f"mode '{entry['mode']}': owner.module '{module}' does not "
                f"exist -- repoint it at whatever implements the mode now, or "
                f"drop the field; a catalogue that names deleted code is fiction")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--registry", type=Path, default=REGISTRY)
    ap.add_argument("--schema", type=Path, default=SCHEMA)
    args = ap.parse_args()

    try:
        label = args.registry.relative_to(REPO).as_posix()
    except ValueError:
        label = str(args.registry)

    try:
        doc = json.loads(args.registry.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"FAIL {label}: parse error ({e})")
        return 1

    validator = _make_validator(args.schema)
    problems = _validate_schema(doc, validator)
    evidence_problems, unevidenced = _check_evidence(doc)
    problems += evidence_problems
    problems += _check_owner_paths(doc)

    if problems:
        print(f"FAIL {label}")
        for p in problems:
            print(f"  · {p}")
        return 1

    print(f"OK   {label}  "
          f"({len(doc['modes'])} emit modes catalogued; "
          f"{unevidenced} with no in-repo evidence)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
