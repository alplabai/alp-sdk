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
* `inter-chip.tsv`, `alif-ospi.tsv`, `alif-ethernet-phy.tsv` are
  not pad-claim maps and are skipped.

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

REPO = Path(__file__).resolve().parent.parent
MODULES = REPO / "metadata" / "e1m_modules"

# Pads that are documented as legitimately dual-claimed by two
# peripherals (typically GD32 + Renesas both routing PWM6/7 / E1M
# PWM dual-source).  Keys are the family-relative TSV path; values
# are sets of "pad" strings that may appear on more than one row.
ALLOWLIST: dict[str, set[str]] = {
    # E1M PWM0..PWM5 are dual-sourced on V2N (both Renesas and GD32
    # can drive them).  The dual-source convention lives in
    # `v2n/README.md` -- update both this allowlist and that doc
    # together if the dual-source set changes.
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
        if not pad or pad == "TBD":
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
        if not silicon_pad or silicon_pad == "TBD":
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


def main() -> int:
    all_errors: list[str] = []
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
