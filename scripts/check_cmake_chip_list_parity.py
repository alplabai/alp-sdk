#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Plain-CMake chip-list parity gate -- fails (exit 1) when the root
`CMakeLists.txt` `ALP_SDK_CHIP_LIST` (the plain-CMake / Yocto
`libalp_chips.a` archive, gated by `ALP_BUILD_CHIPS_ONLY`) drifts from
the drivers that actually exist under `chips/<id>/`.

Root cause (issue #476): `ALP_SDK_CHIP_LIST` was a 17-entry hand-curated
subset while `chips/` shipped ~79 drivers -- a plain-CMake or Yocto
consumer could enable `ALP_SDK_CHIP_<NAME>` for a driver the root list
never generated an option for, so the source silently never entered the
archive.  Required/current SoM drivers (PMIC `da9292`, clock
`clk_5l35023b`, Ethernet PHY `rtl8211fdi`, NPU/wireless sideband
`gd32g553`/`gd32_swd`, ...) were among the silently-dropped set.

Correspondence rule (single-source-of-truth convention):
    chips/<id>/<id>.c        <-->   ALP_SDK_CHIP_LIST entry `<id>`
    chips/<id>/<id>_*.c (>=1) <-->  ALP_SDK_CHIP_LIST entry `<id>`
        (unless `<id>` is in CHIP_LIST_EXCLUDED_WITH_REASON below)

Most drivers are one `chips/<id>/<id>.c` file; a driver split by
subsystem into `chips/<id>/<id>_<subsystem>.c` modules (e.g. `cc3501e`
-- issue #461) still counts as present, and the root `CMakeLists.txt`
globs `chips/<id>/<id>*.c` so both shapes build the same way.

This mirrors check_chip_manifest_parity.py's driver<->manifest gate.
The exclusion allowlist is a RATCHET the same way: a driver in
CHIP_LIST_EXCLUDED_WITH_REASON that has since been added to
ALP_SDK_CHIP_LIST is stale and must be removed from the allowlist, so
an exclusion can't silently outlive the reason it was added for.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Drivers deliberately withheld from the plain-CMake / Yocto
# libalp_chips.a build, with the reason a chips/<id>/ core can't
# (or shouldn't) be built there.  EMPTY today: every chips/<id>/ driver
# on disk is OS-agnostic (calls back into <alp/peripheral.h> only, no
# Zephyr or vendor-SDK header) and compiles against alp_sdk_headers
# alone.  A new entry here must be either genuinely Zephyr-only /
# vendor-toolchain-only, or missing -- this is a RATCHET, not a place
# to silently drop a driver that plain-CMake could build.
CHIP_LIST_EXCLUDED_WITH_REASON: dict[str, str] = {}


def _drivers(root: Path) -> set[str]:
    chips_dir = root / "chips"
    if not chips_dir.is_dir():
        return set()
    found: set[str] = set()
    for d in chips_dir.iterdir():
        if not d.is_dir():
            continue
        # Single-file driver: chips/<id>/<id>.c
        if (d / f"{d.name}.c").is_file():
            found.add(d.name)
            continue
        # Subsystem-split driver: chips/<id>/<id>_<subsystem>.c (>=1 file).
        if any(d.glob(f"{d.name}_*.c")):
            found.add(d.name)
    return found


def _cmake_chip_list(root: Path) -> set[str]:
    cmakelists = root / "CMakeLists.txt"
    text = cmakelists.read_text(encoding="utf-8")
    m = re.search(r"set\(ALP_SDK_CHIP_LIST\s*(.*?)\)", text, re.DOTALL)
    if not m:
        print(
            f"error: no set(ALP_SDK_CHIP_LIST ...) block found in {cmakelists}",
            file=sys.stderr,
        )
        sys.exit(2)
    body = m.group(1)
    # Strip the trailing CACHE INTERNAL "..." docstring argument, then
    # tokenize the remaining bare chip-id words.
    body = re.sub(r'CACHE\s+INTERNAL\s+".*"\s*$', "", body, flags=re.DOTALL)
    return set(body.split())


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO, help="repo root (for tests)")
    args = ap.parse_args()

    drivers = _drivers(args.root)
    cmake_list = _cmake_chip_list(args.root)
    excluded = set(CHIP_LIST_EXCLUDED_WITH_REASON)

    errors: list[str] = []

    # Every driver must be in exactly one of {cmake_list, excluded}.
    driver_in_neither = drivers - cmake_list - excluded
    for cid in sorted(driver_in_neither):
        errors.append(
            f"chips/{cid}/{cid}.c is in neither ALP_SDK_CHIP_LIST (CMakeLists.txt) "
            f"nor CHIP_LIST_EXCLUDED_WITH_REASON ({Path(__file__).name}) -- "
            f"add it to ALP_SDK_CHIP_LIST, or exclude it with a documented reason"
        )

    driver_in_both = drivers & cmake_list & excluded
    for cid in sorted(driver_in_both):
        errors.append(
            f"'{cid}' is in both ALP_SDK_CHIP_LIST and CHIP_LIST_EXCLUDED_WITH_REASON "
            f"-- remove it from one"
        )

    # ALP_SDK_CHIP_LIST entries with no backing driver on disk (dead entry).
    for cid in sorted(cmake_list - drivers):
        errors.append(
            f"ALP_SDK_CHIP_LIST lists '{cid}' but chips/{cid}/{cid}.c does not exist "
            f"-- remove it from CMakeLists.txt"
        )

    # Stale exclusion: allowlisted chip no longer exists as a driver at all.
    for cid in sorted(excluded - drivers):
        errors.append(
            f"CHIP_LIST_EXCLUDED_WITH_REASON lists '{cid}' but chips/{cid}/{cid}.c "
            f"does not exist -- remove it from the allowlist in {Path(__file__).name}"
        )

    if errors:
        print("cmake chip-list parity check FAILED:", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    if excluded:
        reasons = ", ".join(
            f"{k} ({v})" for k, v in sorted(CHIP_LIST_EXCLUDED_WITH_REASON.items())
        )
        print(f"note: {len(excluded)} driver(s) excluded from libalp_chips.a: {reasons}")

    print(
        f"cmake chip-list parity OK: {len(drivers)} chips/ drivers, "
        f"{len(cmake_list)} in ALP_SDK_CHIP_LIST, {len(excluded)} excluded."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
