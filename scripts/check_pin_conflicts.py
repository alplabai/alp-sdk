#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Validate that every silicon pad referenced in the SoM peripheral
maps is claimed by **exactly one** peripheral.

The SoM maps live under `metadata/e1m_modules/<family>/*.tsv`.
Two TSV variants exist:

* Pad-first maps (`renesas-peripheral-map.tsv`, `gd32-io-mcu-map.tsv`,
  `m1-additions.tsv`) carry rows of the form `peripheral \t <silicon_pad>`.
  A pad referenced by two `peripheral` cells is a conflict UNLESS
  one of them is on the documented dual-source allowlist below.
* `from-alif.tsv` / `from-cc3501e.tsv` carry rows of the form
  `e1m_pad \t e1m_function \t <silicon_peripheral> \t <silicon_pad>`.
  Conflicts here are between two `silicon_pad` cells claiming
  different `e1m_function`s.
* `inter-chip.tsv`, `alif-ospi.tsv`, `alif-ethernet-phy.tsv` list Alif
  pads under a named column.  They are not checked on their own, but
  they ARE joined with `from-alif.tsv` (`_check_aen_alif_join`): an
  Alif pad claimed by two of these tables is a conflict unless it is on
  `CROSS_TABLE_ALLOWLIST`.  Likewise the V2N base map and
  `v2n-m1/m1-additions.tsv` are joined (`_check_v2n_join`) so an M1
  addition cannot silently reuse a base Renesas pad.

Exit codes:
* 0  -- no conflicts.
* 1  -- one or more conflicts.

Run locally:

    python3 scripts/check_pin_conflicts.py

CI wires this in `pr-metadata-validate.yml`.
"""

from __future__ import annotations

import csv
import sys
from pathlib import Path
from collections import defaultdict

from sentinels import is_tbd

REPO = Path(__file__).resolve().parent.parent
MODULES = REPO / "metadata" / "e1m_modules"

# Pads that are documented as legitimately dual-claimed by two
# peripherals.  Keys are the family-relative TSV path; values are
# sets of "pad" strings that may appear on more than one row.
ALLOWLIST: dict[str, set[str]] = {
    # V2N: all eight E1M PWM channels are GD32-driven; Renesas
    # drives none.  No dual-source pads remain on V2N after the
    # 2026-05-12 design decision; this allowlist stays empty for
    # the family.  See metadata/e1m_modules/v2n/README.md.
}

# Pad-first maps: column[0] = peripheral, column[1] = silicon pad.
PAD_FIRST_TSVS = (
    "v2n/renesas-peripheral-map.tsv",
    "v2n/gd32-io-mcu-map.tsv",
    "v2n-m1/m1-additions.tsv",
)

# E1M-pad-first maps: row[0]=e1m_pad, row[1]=e1m_function,
# row[2]=silicon_peripheral, row[3]=silicon_pad.  Conflict =
# same silicon_pad on two rows with different e1m_function.
E1M_PAD_TSVS = (
    "aen/from-alif.tsv",
    "aen/from-cc3501e.tsv",
)

# AEN Alif-pad tables joined across files: (relative path, column index
# of the Alif pad, column index of the signal name, has an unfiltered
# header row that must be dropped).
AEN_ALIF_JOIN_TSVS = (
    ("aen/from-alif.tsv", 3, 1, False),
    ("aen/inter-chip.tsv", 4, 0, True),
    ("aen/alif-ospi.tsv", 1, 0, True),
    ("aen/alif-ethernet-phy.tsv", 1, 0, True),
)

# V2N pad-first tables joined across files (column[1] = silicon pad).
V2N_JOIN_TSVS = (
    "v2n/renesas-peripheral-map.tsv",
    "v2n-m1/m1-additions.tsv",
)

# Alif pads legitimately claimed by two different AEN tables.  The E1M
# edge SD bus and the CC3501E SDIO link land on the same six Alif pads
# through a board-level mux that time-shares the single Alif SDIO
# controller; the CC3501E side is behind unpopulated 0 ohm links on
# every board built to date.  See docs/cc3501e-bridge.md ("SDIO").
CROSS_TABLE_ALLOWLIST: set[str] = {
    "P13_0",
    "P13_1",
    "P13_2",
    "P13_3",
    "P14_0",
    "P14_1",
}


def _read_rows(path: Path) -> list[list[str]]:
    if not path.is_file():
        return []
    out = []
    with path.open(encoding="utf-8") as fi:
        for line in fi:
            s = line.rstrip("\n")
            if not s or s.startswith("#"):
                continue
            if s.startswith("peripheral\t") or s.startswith("e1m_pad\t"):
                continue
            out.append(s.split("\t"))
    return out


def _check_pad_first(rel: str) -> list[str]:
    """Return a list of error strings for the pad-first TSV at @p rel."""
    path = MODULES / rel
    if not path.is_file():
        return []
    pad_to_peripherals: dict[str, list[str]] = defaultdict(list)
    for row in _read_rows(path):
        if len(row) < 2:
            continue
        peripheral, pad = row[0].strip(), row[1].strip()
        if not pad or is_tbd(pad):
            continue
        pad_to_peripherals[pad].append(peripheral)

    allow = ALLOWLIST.get(rel, set())
    errors: list[str] = []
    for pad, peripherals in pad_to_peripherals.items():
        if len(peripherals) > 1 and pad not in allow:
            errors.append(
                f"{rel}: pad {pad!r} claimed by {len(peripherals)} peripherals: "
                f"{', '.join(repr(p) for p in peripherals)}"
            )
    return errors


def _check_e1m_pad(rel: str) -> list[str]:
    """Return a list of error strings for the e1m-pad-first TSV at @p rel."""
    path = MODULES / rel
    if not path.is_file():
        return []
    # Conflict: same silicon_pad with different e1m_function.
    sipad_to_funcs: dict[str, set[str]] = defaultdict(set)
    for row in _read_rows(path):
        if len(row) < 4:
            continue
        e1m_function, silicon_pad = row[1].strip(), row[3].strip()
        if not silicon_pad or is_tbd(silicon_pad):
            continue
        sipad_to_funcs[silicon_pad].add(e1m_function)
    errors: list[str] = []
    for sipad, funcs in sipad_to_funcs.items():
        if len(funcs) > 1:
            errors.append(
                f"{rel}: silicon pad {sipad!r} mapped to multiple E1M "
                f"functions: {', '.join(repr(f) for f in funcs)}"
            )
    return errors


def _join_errors(
    claims: dict[str, list[tuple[str, str]]], allow: set[str]
) -> list[str]:
    errors: list[str] = []
    for pad, who in claims.items():
        files = {f for f, _ in who}
        if len(files) > 1 and pad not in allow:
            errors.append(
                f"pad {pad!r} claimed across tables: "
                + ", ".join(f"{f}:{sig!r}" for f, sig in who)
            )
    return errors


def _check_aen_alif_join() -> list[str]:
    """Join the AEN Alif-pad tables; a pad in two files is a conflict."""
    claims: dict[str, list[tuple[str, str]]] = defaultdict(list)
    for rel, pad_col, sig_col, has_header in AEN_ALIF_JOIN_TSVS:
        rows = _read_rows(MODULES / rel)
        for row in rows[1:] if has_header else rows:
            if len(row) <= pad_col:
                continue
            pad = row[pad_col].strip()
            if not pad or is_tbd(pad):
                continue
            claims[pad].append((Path(rel).name, row[sig_col].strip()))
    return _join_errors(claims, CROSS_TABLE_ALLOWLIST)


def _check_v2n_join() -> list[str]:
    """Join the V2N base map with the M1 additions."""
    claims: dict[str, list[tuple[str, str]]] = defaultdict(list)
    for rel in V2N_JOIN_TSVS:
        for row in _read_rows(MODULES / rel):
            if len(row) < 2:
                continue
            pad = row[1].strip()
            if not pad or is_tbd(pad):
                continue
            claims[pad].append((Path(rel).name, row[0].strip()))
    return _join_errors(claims, set())


def main() -> int:
    all_errors: list[str] = []
    all_errors.extend(_check_aen_alif_join())
    all_errors.extend(_check_v2n_join())
    for rel in PAD_FIRST_TSVS:
        all_errors.extend(_check_pad_first(rel))
    for rel in E1M_PAD_TSVS:
        all_errors.extend(_check_e1m_pad(rel))

    if all_errors:
        for e in all_errors:
            print(f"pin-conflict: {e}", file=sys.stderr)
        print(
            f"\ncheck_pin_conflicts: {len(all_errors)} conflict(s).  "
            "If a row is a legitimate dual-source map, add the pad to "
            "ALLOWLIST in scripts/check_pin_conflicts.py and update the "
            "matching README to document the dual-source convention.",
            file=sys.stderr,
        )
        return 1
    print("check_pin_conflicts: all pad claims are unique.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
