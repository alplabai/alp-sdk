#!/usr/bin/env python3
# Copyright 2026 ALP Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Drop the GigaDevice **GD32G5x3 Firmware Library** archive into
`vendors/gd32_firmware_library/sdk/`, following the layout documented
in that directory's README.md.

Usage
-----

    python3 scripts/drop_gd32_firmware_library.py <path-to-archive>

`<path-to-archive>` is the .zip / .rar / .7z the GigaDevice MCU portal
serves at <https://www.gd32mcu.com/en/download/7?kw=GD32G5> -- the
"GD32G5x3 Firmware Library" asset (v1.5.0 as of 2026-05-13).  The
script:

  * verifies the archive structure looks like the firmware library
    (looks for `Firmware/CMSIS/GD/GD32G5x3/Include/gd32g5x3.h`),
  * wipes any prior `vendors/gd32_firmware_library/sdk/` contents,
  * extracts `Firmware/` (mandatory) + `Utilities/` (if present) into
    the canonical drop location,
  * prints the resolved version triple from `gd32g5x3.h` so you can
    confirm what landed.

Refuses to write anything if the archive doesn't carry the expected
top-level shape -- the script doesn't try to be clever about
heuristic layouts; bad archive = clear error message.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import shutil
import sys
import tempfile
import zipfile
from typing import Optional


ROOT          = pathlib.Path(__file__).resolve().parent.parent
DROP_ROOT     = ROOT / "vendors" / "gd32_firmware_library" / "sdk"
DEVICE_HEADER = "Firmware/CMSIS/GD/GD32G5x3/Include/gd32g5x3.h"


def _find_subdir_with_anchor(extract_root: pathlib.Path,
                             anchor_rel: str) -> Optional[pathlib.Path]:
    """Search up to three levels deep for a directory containing `anchor_rel`."""
    candidates = [extract_root]
    for depth in range(3):
        next_candidates: list[pathlib.Path] = []
        for cand in candidates:
            if (cand / anchor_rel).exists():
                return cand
            if cand.is_dir():
                next_candidates.extend(c for c in cand.iterdir() if c.is_dir())
        candidates = next_candidates
    return None


def _parse_library_version(header_path: pathlib.Path) -> Optional[str]:
    """Pull the V_MAIN / V_SUB1 / V_SUB2 / V_RC fields out of the device header."""
    try:
        text = header_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    # The vendor's headers declare `#define __GD32G5X3_STDPERIPH_VERSION_MAIN`
    # plus _SUB1 / _SUB2 / _RC.  Concatenate the integers we find for a
    # human-readable version string.
    parts = []
    for tag in ("MAIN", "SUB1", "SUB2", "RC"):
        m = re.search(rf"#define\s+__GD32G5X3_STDPERIPH_VERSION_{tag}\s+\(?\s*0[xX]([0-9a-fA-F]+)",
                      text)
        if not m:
            return None
        parts.append(str(int(m.group(1), 16)))
    return ".".join(parts)


def _extract_archive(archive: pathlib.Path,
                     dest: pathlib.Path) -> None:
    """Extract `archive` into `dest`.  Currently supports .zip; .rar / .7z
    surface a clear error so the user knows to convert first."""
    suffix = archive.suffix.lower()
    if suffix == ".zip":
        with zipfile.ZipFile(archive, "r") as zf:
            zf.extractall(dest)
        return
    raise SystemExit(
        f"error: unsupported archive type '{suffix}'.  "
        f"GigaDevice's portal sometimes serves .rar -- if so, re-pack "
        f"as .zip locally and rerun."
    )


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=pathlib.Path,
                        help="path to the GigaDevice 'GD32G5x3 Firmware Library' archive")
    parser.add_argument("--force", action="store_true",
                        help="overwrite an existing drop without prompting")
    args = parser.parse_args(argv)

    archive: pathlib.Path = args.archive.resolve()
    if not archive.is_file():
        print(f"error: archive not found: {archive}", file=sys.stderr)
        return 1

    print(f"==> extracting {archive.name}")

    with tempfile.TemporaryDirectory(prefix="gd32_fw_") as scratch_dir:
        scratch = pathlib.Path(scratch_dir)
        try:
            _extract_archive(archive, scratch)
        except SystemExit:
            raise
        except Exception as exc:
            print(f"error: extraction failed: {exc}", file=sys.stderr)
            return 1

        # The vendor archive nests `Firmware/` two or three levels deep
        # (a top-level wrapper folder named like the archive version,
        # sometimes another inner folder, then `Firmware/`).  Walk a
        # short search depth instead of hardcoding.
        sdk_root = _find_subdir_with_anchor(scratch, DEVICE_HEADER)
        if sdk_root is None:
            print(
                f"error: archive does not look like the GD32G5x3 Firmware Library; "
                f"could not locate {DEVICE_HEADER!s} within the first three nesting levels.\n"
                f"Confirm you downloaded the 'GD32G5x3 Firmware Library' asset "
                f"(not the AddOn / DFP pack) from "
                f"https://www.gd32mcu.com/en/download/7?kw=GD32G5",
                file=sys.stderr,
            )
            return 1

        # Confirm what version we picked up so the next step doesn't get
        # ambiguous.  Older v1.4.x and the current v1.5.0 share the
        # same layout; the header tells us which.
        version_str = _parse_library_version(sdk_root / DEVICE_HEADER)
        if version_str:
            print(f"==> detected GD32G5x3 firmware library version: {version_str}")
        else:
            print(f"warn: couldn't parse the library version out of "
                  f"{DEVICE_HEADER}; proceeding anyway", file=sys.stderr)

        if DROP_ROOT.exists() and not args.force:
            existing_top = ", ".join(sorted(p.name for p in DROP_ROOT.iterdir()))
            print(
                f"error: drop location already populated ({DROP_ROOT}).\n"
                f"        contains: {existing_top or '(empty)'}\n"
                f"Rerun with --force to overwrite, or delete the directory by hand.",
                file=sys.stderr,
            )
            return 1

        # Clean + recreate the drop location.
        if DROP_ROOT.exists():
            shutil.rmtree(DROP_ROOT)
        DROP_ROOT.mkdir(parents=True, exist_ok=True)

        print(f"==> copying Firmware/ into {DROP_ROOT}")
        shutil.copytree(sdk_root / "Firmware", DROP_ROOT / "Firmware")

        utilities = sdk_root / "Utilities"
        if utilities.exists():
            print(f"==> copying Utilities/ into {DROP_ROOT}")
            shutil.copytree(utilities, DROP_ROOT / "Utilities")
        else:
            print("==> archive has no Utilities/ subtree (fine -- it's optional)")

    # Sanity-check the post-drop state matches what the bridge build expects.
    device_h = DROP_ROOT / DEVICE_HEADER
    if not device_h.exists():
        print(f"error: post-copy sanity check failed -- {device_h} not present",
              file=sys.stderr)
        return 1

    print()
    print(f"OK: GD32 firmware library dropped under {DROP_ROOT.relative_to(ROOT)}/")
    print()
    print("Next steps:")
    print("  cd gd32-bridge")
    print("  cmake -B build \\")
    print("        -DCMAKE_TOOLCHAIN_FILE=toolchain/arm-none-eabi.cmake \\")
    print("        -DBRIDGE_HAL_BACKEND=gd32")
    print("  cmake --build build")
    return 0


if __name__ == "__main__":
    sys.exit(main())
