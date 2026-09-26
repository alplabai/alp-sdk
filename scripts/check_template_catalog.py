#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Validate metadata/templates/catalog-v1.json against the public template
contract (metadata/schemas/template-catalog-v1.schema.json) AND drift-check
every record against the real tree.

The catalog is the single source of truth for "what starter project
archetypes does the SDK ship, and what does each one need" -- a frontend
(alp-sdk-vscode, Alp Studio, a future `alp new`) reads THIS instead of
guessing from directory listings under examples/. Each record maps to a
CANONICAL existing example; nothing here is a copy of that example, so
nothing here may silently drift from it. This gate fails when:

  - the catalog doesn't validate against the schema;
  - a record's `example` path doesn't exist or has no board.yaml;
  - a record's `test.testcase_yaml` path(s) don't exist;
  - a record's `requires.libraries` names a library the SDK doesn't
    actually recognise (cross-checked against
    metadata/library-aliases-v1.json, the curated `libraries:` vocabulary --
    legacy tokens and canonical manifest names alike);
  - a record's `requires.chips` names a chip with no
    metadata/chips/<name>.yaml manifest;
  - a record's `requires.portable_apis` names a header that doesn't exist
    under include/alp/;
  - a `stable` record's `example` is one of the tracked content-quality
    gaps in issue #448 (iot-connected-camera documented as v1.0-ready
    while its own runtime/README/testcase.yaml still describe a v0.1/v0.2
    skeleton) or issue #520 (six display/LVGL examples plus
    iot-dashboard bypass <alp/display.h>/<alp/gui.h> for a direct
    zephyr/drivers/display.h include) -- such an example may only be
    referenced by a `preview` record with a `note`, never `stable`.
  - a record's `cores` field (issue #1275) disagrees with its canonical
    example's own board.yaml `cores:` mapping -- same ids, same `app:`
    dirs, same EFFECTIVE `os:` (resolved via alp_orchestrate.load_board_
    yaml, not re-derived here). `cores` duplicates what board.yaml
    already says; this is what keeps that duplication from drifting.
  - a file git tracks in one of a record's `cores[].dir` app dirs is
    missing from `files.user_owned` (issue #2241). `--emit scaffold`
    copies exactly that list, so an omitted file silently vanishes from
    every scaffold -- iot shipped without src/cc3501e_bridge.{c,h} while
    its CMakeLists.txt still compiled the .c. A core dir's testcase.yaml
    is exempt when `test.testcase_yaml` lists it (SDK CI wiring the
    scaffold drops on purpose).

Run locally:

    python3 scripts/check_template_catalog.py
    python3 scripts/check_template_catalog.py --catalog path/to/catalog.json
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path

import jsonschema
import yaml

REPO = Path(__file__).resolve().parent.parent
CATALOG = REPO / "metadata" / "templates" / "catalog-v1.json"
SCHEMA = REPO / "metadata" / "schemas" / "template-catalog-v1.schema.json"
INCLUDE_ALP = REPO / "include" / "alp"
CHIPS_METADATA = REPO / "metadata" / "chips"

sys.path.insert(0, str(REPO / "scripts"))

from alp_orchestrate import OrchestratorError, load_board_yaml  # noqa: E402

# Examples with a TRACKED content-quality gap -- grounded in the two issues
# named in the module docstring. A `stable` record may never point at one of
# these; it must be `preview` (or the record dropped) with a `note` citing
# the issue. This is a small, explicit allowlist (not a live `gh issue`
# query) so the gate has no network dependency; keep it in sync if either
# issue's example list changes.
KNOWN_GAP_EXAMPLES: dict[str, str] = {
    # #448: presented as a v1.0-ready flagship, but src/main.c still prints
    # "v0.1 skeleton", testcase.yaml describes v0.2/v0.3 stubs, and
    # check_example_portability.py reports it ring-unknown.
    "examples/connectivity/iot-connected-camera": "#448",
    # #520: opens Zephyr's display device directly
    # (zephyr/drivers/display.h + DT_CHOSEN(zephyr_display)) instead of
    # <alp/display.h>/<alp/gui.h>.
    "examples/camera-vision/ai-camera-viewer": "#520",
    "examples/connectivity/iot-dashboard": "#520",
    "examples/display/drone-hud": "#520",
    "examples/display/lvgl-benchmark": "#520",
    "examples/display/lvgl-music-player": "#520",
    "examples/display/lvgl-widgets-demo": "#520",
}


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


def _real_library_names() -> set[str]:
    """The real curated `libraries:` vocabulary board.yaml accepts -- read from
    metadata/library-aliases-v1.json so this can't drift from the manifests it
    maps to.  Accepts both the legacy per-core token (alias key) and the
    canonical manifest name (alias value)."""
    doc = json.loads(
        (REPO / "metadata" / "library-aliases-v1.json").read_text(encoding="utf-8"))
    aliases = doc.get("aliases") or {}
    return set(aliases.keys()) | set(aliases.values())


def _check_record_drift(rec: dict, real_libraries: set[str]) -> list[str]:
    problems: list[str] = []
    rid = rec.get("id", "<no id>")

    example = REPO / rec["example"]
    if not example.is_dir():
        problems.append(f"{rid}: example {rec['example']!r} does not exist")
    elif not (example / "board.yaml").is_file():
        problems.append(f"{rid}: example {rec['example']!r} has no board.yaml")

    for tc in rec["test"]["testcase_yaml"]:
        if not (REPO / tc).is_file():
            problems.append(f"{rid}: test.testcase_yaml {tc!r} does not exist")

    for header in rec["requires"]["portable_apis"]:
        if not (REPO / header).is_file():
            problems.append(
                f"{rid}: requires.portable_apis {header!r} does not exist")

    for lib in rec["requires"]["libraries"]:
        if lib not in real_libraries:
            problems.append(
                f"{rid}: requires.libraries {lib!r} is not a real library "
                f"(not in metadata/library-aliases-v1.json)")

    for chip in rec["requires"]["chips"]:
        if not (CHIPS_METADATA / f"{chip}.yaml").is_file():
            problems.append(
                f"{rid}: requires.chips {chip!r} has no "
                f"metadata/chips/{chip}.yaml manifest")

    gap_issue = KNOWN_GAP_EXAMPLES.get(rec["example"])
    if gap_issue and rec["status"] == "stable":
        problems.append(
            f"{rid}: status is 'stable' but example {rec['example']!r} is a "
            f"tracked content-quality gap ({gap_issue}) -- mark 'preview' "
            f"with a note, or point at a different canonical example")

    return problems


def _check_cores_match_board_yaml(rec: dict) -> list[str]:
    """A record's `cores` field duplicates its canonical example's own
    board.yaml `cores:` mapping (issue #1275) -- assert they agree
    EXACTLY (same ids, same `app:` dirs, same effective `os:`) so the
    duplication is machine-checked, not drift-prone.

    `os` is resolved via `alp_orchestrate.load_board_yaml` -- the same
    loader `--emit zephyr-conf` itself uses -- rather than re-derived
    here, so a board.yaml core with no explicit `os:` (inferred from
    its SoC core class) is compared against the REAL effective value,
    not a guess. Only ids the board.yaml's own `cores:` mapping
    declares are compared (never the full SoM topology, which also
    includes cores the project never mentions, e.g. a second M-class
    core left at its stock-shim default)."""
    rid = rec.get("id", "<no id>")
    board_yaml = REPO / rec["example"] / "board.yaml"
    if not board_yaml.is_file():
        return []  # already reported by _check_record_drift

    raw = yaml.safe_load(board_yaml.read_text(encoding="utf-8")) or {}
    raw_cores = raw.get("cores") or {}

    try:
        project = load_board_yaml(board_yaml)
    except OrchestratorError as e:
        return [f"{rid}: cores: board.yaml failed to load ({e})"]

    expected: dict[str, dict] = {}
    for core_id, entry in raw_cores.items():
        slice_ = project.cores.get(core_id)
        if slice_ is None:
            continue  # loader already rejects an unmatched core id
        app = (entry or {}).get("app")
        expected[core_id] = {
            "id": core_id,
            **({"dir": app} if app else {}),
            "os": slice_.os,
        }

    declared = {c["id"]: c for c in rec.get("cores", [])}

    problems: list[str] = []
    missing = sorted(set(expected) - set(declared))
    if missing:
        problems.append(
            f"{rid}: cores is missing {missing} -- {rec['example']}/"
            f"board.yaml `cores:` declares {sorted(expected)}")
    extra = sorted(set(declared) - set(expected))
    if extra:
        problems.append(
            f"{rid}: cores declares {extra}, which {rec['example']}/"
            f"board.yaml `cores:` does not")
    for core_id in sorted(set(expected) & set(declared)):
        if expected[core_id] != declared[core_id]:
            problems.append(
                f"{rid}: cores entry for {core_id!r} is "
                f"{declared[core_id]} but {rec['example']}/board.yaml "
                f"resolves to {expected[core_id]} -- drift")
    return problems


def _tracked_files_under(rel_dir: str) -> list[str]:
    """Repo-relative POSIX paths git tracks under `rel_dir`. Tracked, not
    walked: a stray local file (.DS_Store, an editor swap file) must never
    read as an undeclared template source."""
    out = subprocess.run(
        ["git", "-C", str(REPO), "ls-files", "--", rel_dir],
        capture_output=True, text=True, encoding="utf-8", check=True)
    return out.stdout.splitlines()


def _check_core_files_declared(rec: dict) -> list[str]:
    """Every tracked file in a record's core app dirs must be in
    `files.user_owned` (issue #2241) -- `--emit scaffold` copies exactly
    that list, so anything it omits is silently missing from the scaffold.
    Exempt: a path `test.testcase_yaml` lists (CI wiring, never scaffolded).
    A core whose `dir` is the example root is not walked: the root also
    holds CI-only files (native_sim.conf, boards/ overlays) by design."""
    rid = rec.get("id", "<no id>")
    example = rec["example"].rstrip("/")
    declared = set(rec["files"]["user_owned"])
    ci_only = set(rec["test"]["testcase_yaml"])

    problems: list[str] = []
    for core in rec.get("cores", []):
        core_dir = (core.get("dir") or "").removeprefix("./").strip("/")
        if core_dir in ("", "."):
            continue
        for path in _tracked_files_under(f"{example}/{core_dir}"):
            rel = path.removeprefix(f"{example}/")
            if path in ci_only or rel in declared:
                continue
            problems.append(
                f"{rid}: {path} is in the {core['id']} core's app dir but "
                f"not in files.user_owned -- `--emit scaffold` would drop it")
    return problems


def _check_ids_unique(doc: dict) -> list[str]:
    ids = [t["id"] for t in doc.get("templates", [])]
    dupes = sorted({i for i in ids if ids.count(i) > 1})
    if dupes:
        return [f"duplicate template id(s): {dupes}"]
    return []


def _check_archetypes_covered(doc: dict) -> list[str]:
    """Every declared archetype enum value should have >=1 record -- the
    catalog's whole point is one canonical template per archetype."""
    schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
    all_archetypes = set(
        schema["$defs"]["template"]["properties"]["archetype"]["enum"])
    present = {t["archetype"] for t in doc.get("templates", [])}
    missing = sorted(all_archetypes - present)
    if missing:
        return [f"archetype(s) with no template record: {missing}"]
    return []


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--catalog", type=Path, default=CATALOG)
    ap.add_argument("--schema", type=Path, default=SCHEMA)
    args = ap.parse_args()

    try:
        label = args.catalog.relative_to(REPO).as_posix()
    except ValueError:
        label = str(args.catalog)

    try:
        doc = json.loads(args.catalog.read_text(encoding="utf-8"))
    except Exception as e:
        print(f"FAIL {label}: parse error ({e})")
        return 1

    validator = _make_validator(args.schema)
    problems = _validate_schema(doc, validator)

    if not problems:
        # Only run the drift/semantic checks once the doc is schema-valid --
        # they assume the required keys are present.
        real_libraries = _real_library_names()
        for rec in doc.get("templates", []):
            problems += _check_record_drift(rec, real_libraries)
            problems += _check_cores_match_board_yaml(rec)
            problems += _check_core_files_declared(rec)
        problems += _check_ids_unique(doc)
        problems += _check_archetypes_covered(doc)

    if problems:
        print(f"FAIL {label}")
        for p in problems:
            print(f"  · {p}")
        return 1

    print(f"OK   {label}  ({len(doc['templates'])} templates, no drift)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
