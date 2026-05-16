# SPDX-License-Identifier: Apache-2.0
"""
Regression test for the cross-SoM coverage of `alp_core_id_t`
(declared in include/alp/mproc.h).

Every core_id that appears in any shipped SoM preset's `topology:`
block MUST have a matching `ALP_CORE_<CORE_ID_UPPER>` enum entry in
the C header.  Without this guard, a new SoM SKU could land its
preset + topology defaults but silently ship a C API that can't
address its M-core or A-cluster -- `<alp/mproc.h>` would only know
about the older SoMs' cores.

Maintenance note: if this test fails after adding a new SoM, the
fix is almost always to extend the enum in `include/alp/mproc.h`
with a fresh integer value at the END of the enum (never re-number
existing entries -- the ABI promise locks them).  The enum's
docstring at mproc.h documents the convention.

Run locally:

    python -m pytest tests/scripts/test_core_id_enum_coverage.py -v
"""

from __future__ import annotations

import re
from pathlib import Path

import pytest
import yaml


REPO = Path(__file__).resolve().parents[2]
MPROC_H = REPO / "include" / "alp" / "mproc.h"
SOM_DIR = REPO / "metadata" / "e1m_modules"


# ---------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------


def _topology_core_ids() -> set[str]:
    """Collect every core_id that appears in any SoM preset's
    `topology:` block.  Uses the *.yaml presets directly under
    metadata/e1m_modules/ (skips per-family subdirectories that
    carry hw-revisions.yaml etc.)."""
    ids: set[str] = set()
    for sf in SOM_DIR.glob("E1M-*.yaml"):
        with sf.open(encoding="utf-8") as fh:
            preset = yaml.safe_load(fh) or {}
        topology = preset.get("topology") or {}
        for core_id in topology.keys():
            ids.add(core_id)
    return ids


def _enum_entries() -> dict[str, int]:
    """Parse `alp_core_id_t` and return a {NAME: VALUE} map (without
    the ALP_CORE_ prefix, lower-cased to match topology keys).

    Tolerates the C enum's comment blocks + // line comments;
    rejects malformed entries with a clear error."""
    text = MPROC_H.read_text(encoding="utf-8")
    m = re.search(
        r"typedef\s+enum\s*\{(?P<body>.*?)\}\s*alp_core_id_t",
        text, flags=re.DOTALL,
    )
    assert m, (
        "Could not locate `typedef enum { ... } alp_core_id_t;` in "
        f"{MPROC_H}.  Did the enum get renamed?"
    )
    body = m.group("body")

    # Strip /* … */ and // … comments before parsing.
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.DOTALL)
    body = re.sub(r"//[^\n]*", "", body)

    entries: dict[str, int] = {}
    for line in body.splitlines():
        line = line.strip().rstrip(",")
        if not line:
            continue
        m2 = re.match(r"ALP_CORE_(?P<name>[A-Z0-9_]+)\s*=\s*(?P<val>\d+)", line)
        assert m2, f"malformed enum entry: {line!r}"
        entries[m2.group("name").lower()] = int(m2.group("val"))
    return entries


# ---------------------------------------------------------------------
# 1. Every topology core_id has an enum entry
# ---------------------------------------------------------------------


def test_every_topology_core_id_has_enum_entry() -> None:
    """For every (SoM, core_id) the orchestrator might resolve, the C
    enum must carry a matching entry.  Catches the failure mode where
    a new SoM lands its preset but the C API forgets to enumerate
    its cores."""
    topology_ids = _topology_core_ids()
    enum_names = set(_enum_entries().keys())

    # `self` is a synthetic enum value (the running core) -- it doesn't
    # appear in topology by definition.  Strip it from the diff.
    enum_names.discard("self")

    missing = topology_ids - enum_names
    assert not missing, (
        f"\nTopology core_ids without ALP_CORE_<NAME> enum entries in "
        f"{MPROC_H.relative_to(REPO).as_posix()}:\n"
        + "\n".join(f"  - {x}" for x in sorted(missing))
        + "\n\nFix: extend the enum at the END (preserve existing "
          "integer values for ABI back-compat) and re-run."
    )


# ---------------------------------------------------------------------
# 2. ABI stability: known values stay pinned
# ---------------------------------------------------------------------


# The integer values for entries that shipped in v0.3.  Extending the
# enum is allowed; re-numbering these is forbidden.  Adding a new SoM
# SHOULD append to the enum with a fresh integer, never reuse one
# of these slots.
_FROZEN_ASSIGNMENTS = {
    "self":     0,
    "m55_hp":   1,
    "m55_he":   2,
    "a32_0":    3,
    "a32_1":    4,
}


@pytest.mark.parametrize("name,expected", sorted(_FROZEN_ASSIGNMENTS.items()))
def test_frozen_enum_value_unchanged(name: str, expected: int) -> None:
    """ABI lock: the v0.3-shipped integer values are immutable.  If
    this test fails, an enum entry has been renumbered -- existing
    callers compiled against the old value would silently address
    the wrong core."""
    entries = _enum_entries()
    assert name in entries, (
        f"ALP_CORE_{name.upper()} disappeared from the enum -- "
        "removing entries is an ABI break."
    )
    assert entries[name] == expected, (
        f"ALP_CORE_{name.upper()} changed integer value from "
        f"{expected} to {entries[name]} -- ABI break."
    )


# ---------------------------------------------------------------------
# 3. Enum values are unique (no accidental aliasing)
# ---------------------------------------------------------------------


def test_enum_values_unique() -> None:
    """Every enum entry must carry a distinct integer.  Catches the
    case where someone adds a new core and reuses a value from the
    frozen-assignment list above."""
    entries = _enum_entries()
    seen: dict[int, str] = {}
    collisions: list[tuple[str, str, int]] = []
    for name, val in entries.items():
        if val in seen:
            collisions.append((seen[val], name, val))
        else:
            seen[val] = name
    assert not collisions, (
        "Duplicate integer values in alp_core_id_t:\n"
        + "\n".join(f"  {a} and {b} both = {v}" for a, b, v in collisions)
    )


# ---------------------------------------------------------------------
# 4. Cross-check against SoM topology contract
# ---------------------------------------------------------------------


def test_every_som_preset_topology_resolves() -> None:
    """Smoke: each SoM preset's topology is non-empty (catches a
    preset that forgets the topology block entirely -- the
    orchestrator would silently treat it as a single-core build)."""
    presets = sorted(SOM_DIR.glob("E1M-*.yaml"))
    assert presets, f"no E1M-*.yaml presets found in {SOM_DIR}"
    empty: list[str] = []
    for sf in presets:
        preset = yaml.safe_load(sf.read_text(encoding="utf-8")) or {}
        topology = preset.get("topology") or {}
        if not topology:
            empty.append(sf.name)
    assert not empty, (
        "SoM presets with missing/empty topology:\n"
        + "\n".join(f"  - {x}" for x in empty)
    )
