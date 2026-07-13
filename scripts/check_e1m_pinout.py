#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Validate the vendored e1m-spec pinout snapshot and cross-check it against
the per-SoM pin-mux tables -- fully **offline** (no network).

The snapshot (metadata/e1m/pinout-*.json) is a verbatim copy of the E1M
standard's machine-readable pinout, owned by `alplabai/e1m-spec` and
refreshed by scripts/sync_e1m_spec.py.  This gate is the reason the
snapshot earns its place: it turns "look the pad up in another repo" into
an enforced invariant.

Checks:

  1. schema     -- each snapshot validates against the vendored upstream
                   `loom-v1` schema (metadata/schemas/loom-v1.schema.json).
  2. pad ids    -- pad ids are unique within each snapshot.
  3. lock       -- metadata/e1m/e1m-spec.lock is present and its declared
                   {id, version, pad_count} match each snapshot (ratchet:
                   a snapshot swapped without updating the lock fails).
  4. join       -- every non-"TBD" `e1m_pad` in metadata/pinmux/<family>.yaml
                   resolves to a pad in the matching snapshot AND that pad's
                   `silkscreen` equals the row's `e1m_function`.  This is the
                   independent, gated link between the per-SoM TSVs and the
                   E1M standard.

The snapshot is regenerated, never hand-edited -- so a failure here means
either a stale snapshot (re-run sync_e1m_spec.py) or a TSV that names a pad
or function the standard doesn't define (fix the TSV).

Exit codes: 0 -- all checks pass; 1 -- one or more failures.

Run locally:

    python3 scripts/check_e1m_pinout.py

CI wires this in .github/workflows/pr-metadata-validate.yml.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import yaml

try:
    import jsonschema
except ImportError:  # pragma: no cover - dependency is in CI + dev env
    sys.exit("error: jsonschema not installed -- `pip install jsonschema`.")

REPO = Path(__file__).resolve().parent.parent
E1M_DIR = REPO / "metadata" / "e1m"
PINMUX_DIR = REPO / "metadata" / "pinmux"
SCHEMA_PATH = REPO / "metadata" / "schemas" / "loom-v1.schema.json"
LOCK_PATH = E1M_DIR / "e1m-spec.lock"

# Snapshot files by form-factor id.
SNAPSHOTS = {
    "E1M": E1M_DIR / "pinout-v1.json",
    "E1M-X": E1M_DIR / "pinout-x-v1.json",
}

# Which E1M form factor each pin-mux family targets.  E1M (35x35) vs
# E1M-X (45x65) share pad-coordinate names (both have an "A3"), so the
# join target can't be inferred from the pad id alone -- it is fixed by
# the module family.  The repo's own convention detects E1M-X by the
# `E1M_X_` route prefix (see scripts/gen_board_header.py); AEN is E1M,
# V2N is E1M-X.  A pin-mux family absent from this map is a hard error.
FAMILY_FORM_FACTOR = {
    "aen": "E1M",
    "v2n": "E1M-X",
}


def _fail(errors: list[str], msg: str) -> None:
    errors.append(msg)


def _load_snapshot(form: str, errors: list[str]) -> dict | None:
    path = SNAPSHOTS[form]
    if not path.exists():
        _fail(errors, f"[snapshot] missing {path.relative_to(REPO)}")
        return None
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError as exc:
        _fail(errors, f"[snapshot] {path.relative_to(REPO)} is not valid JSON: {exc}")
        return None


def check_schema(snapshots: dict[str, dict], errors: list[str]) -> None:
    try:
        schema = json.loads(SCHEMA_PATH.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        _fail(errors, f"[schema] cannot load {SCHEMA_PATH.relative_to(REPO)}: {exc}")
        return
    validator = jsonschema.Draft202012Validator(schema)
    for form, doc in snapshots.items():
        found = sorted(validator.iter_errors(doc), key=lambda e: list(e.path))
        for e in found:
            loc = "/".join(str(p) for p in e.path) or "<root>"
            _fail(errors, f"[schema] {form}: {loc}: {e.message}")


def check_unique_pad_ids(snapshots: dict[str, dict], errors: list[str]) -> None:
    for form, doc in snapshots.items():
        seen: set[str] = set()
        for pad in doc["pads"]:
            pid = pad["id"]
            if pid in seen:
                _fail(errors, f"[pad-ids] {form}: duplicate pad id {pid!r}")
            seen.add(pid)


def check_lock(snapshots: dict[str, dict], errors: list[str]) -> None:
    if not LOCK_PATH.exists():
        _fail(errors, f"[lock] missing {LOCK_PATH.relative_to(REPO)}")
        return
    try:
        lock = yaml.safe_load(LOCK_PATH.read_text())
    except yaml.YAMLError as exc:
        _fail(errors, f"[lock] {LOCK_PATH.relative_to(REPO)} is not valid YAML: {exc}")
        return
    sha = (lock.get("source") or {}).get("sha")
    if not sha:
        _fail(errors, "[lock] source.sha is missing")
    by_snapshot = {entry.get("snapshot"): entry for entry in lock.get("files", [])}
    for form, doc in snapshots.items():
        rel = str(SNAPSHOTS[form].relative_to(REPO))
        entry = by_snapshot.get(rel)
        if entry is None:
            _fail(errors, f"[lock] no files[] entry for {rel}")
            continue
        for key, actual in (("id", doc["id"]), ("version", doc["version"]),
                            ("pad_count", len(doc["pads"]))):
            declared = entry.get(key)
            if str(declared) != str(actual):
                _fail(errors,
                      f"[lock] {rel}: {key} declared {declared!r} != snapshot {actual!r} "
                      f"(re-run scripts/sync_e1m_spec.py)")


def check_join(snapshots: dict[str, dict], errors: list[str]) -> None:
    for pinmux_path in sorted(PINMUX_DIR.glob("*.yaml")):
        doc = yaml.safe_load(pinmux_path.read_text())
        family = doc.get("family")
        form = FAMILY_FORM_FACTOR.get(family)
        if form is None:
            _fail(errors,
                  f"[join] {pinmux_path.name}: family {family!r} not in FAMILY_FORM_FACTOR "
                  f"-- add its E1M form factor to scripts/check_e1m_pinout.py")
            continue
        snapshot = snapshots.get(form)
        if snapshot is None:
            continue  # snapshot load already reported
        silk = {pad["id"]: pad.get("silkscreen") for pad in snapshot["pads"]}

        total = joined = 0
        for row in doc.get("pads", []):
            pad = row.get("e1m_pad", "TBD")
            if pad == "TBD":
                continue
            total += 1
            fn = row.get("e1m_function")
            if pad not in silk:
                _fail(errors,
                      f"[join] {pinmux_path.name}: pad {pad!r} not defined in {form} standard")
                continue
            if silk[pad] != fn:
                _fail(errors,
                      f"[join] {pinmux_path.name}: pad {pad!r} function {fn!r} != "
                      f"standard silkscreen {silk[pad]!r}")
                continue
            joined += 1
        print(f"  join {pinmux_path.name} ({family} -> {form}): {joined}/{total}")


def main() -> int:
    errors: list[str] = []

    snapshots: dict[str, dict] = {}
    for form in SNAPSHOTS:
        doc = _load_snapshot(form, errors)
        if doc is not None:
            snapshots[form] = doc

    if snapshots:
        check_schema(snapshots, errors)
        check_unique_pad_ids(snapshots, errors)
        check_lock(snapshots, errors)
        check_join(snapshots, errors)

    if errors:
        print(f"\ncheck_e1m_pinout: {len(errors)} error(s):", file=sys.stderr)
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        return 1

    pads = ", ".join(f"{form} {len(doc['pads'])}" for form, doc in snapshots.items())
    print(f"check_e1m_pinout: OK ({pads} pads)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
