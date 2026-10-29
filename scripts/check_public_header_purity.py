#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: root public headers stay chip-neutral.

A header directly under ``include/alp/`` (the portable public API namespace)
must not ``#include <alp/chips/*>``.  Pulling a concrete companion/sensor
chip surface into a root header leaks a vendor type into what apps treat as
the portable API and makes the surface impossible to keep source-compatible
across SoM families (see issue #517: ``<alp/console.h>`` used to expose
``cc3501e_t``).

Chip-specific attach hooks belong behind an extension header under
``include/alp/ext/<companion>/*.h`` (e.g. ``<alp/ext/cc3501e/console.h>``),
which callers include only once they've committed to that silicon.

Scope: only the *top-level* ``include/alp/*.h`` files are checked.  The
``ext/`` and ``chips/`` subtrees are exempt by design -- those are the
explicitly non-portable surfaces.  A narrow ``ALLOWLIST`` covers any root
header that legitimately re-exports a chip type as a documented
protocol/extension surface (empty today).

Second check (C++ purity): every header under ``include/alp/`` must also
parse as C++17 -- a bare ``_Static_assert`` or similar C-only construct
breaks every C++ consumer (use ``ALP_STATIC_ASSERT`` from
``<alp/static_assert.h>``).  Each header is compiled with
``g++ -std=c++17 -fsyntax-only``.  It runs only with ``--cxx`` (the
default run stays hermetic for the quick profile); CI passes ``--cxx
--require-cxx`` so a missing compiler fails instead of skipping.
ALP_BOARD_E1M_EVK is defined so <alp/board.h> resolves.  Headers that
need more are listed in ``CXX_SKIP``
with a reason.
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

CHIP_INCLUDE_RE = re.compile(
    r'^\s*#\s*include\s*[<"]alp/chips/[^">]+[">]', re.MULTILINE
)

# Root headers explicitly permitted to re-export a chip surface, each with a
# justification.  Keep this list empty unless a documented protocol/extension
# surface genuinely must live at the root; prefer include/alp/ext/ instead.
ALLOWLIST: dict[str, str] = {}


# Headers excluded from the C++ syntax check, each with a justification.
CXX_SKIP: dict[str, str] = {}

CXX_COMPILERS = ("g++", "arm-none-eabi-g++", "c++", "clang++")


def find_cxx() -> str | None:
    for cc in CXX_COMPILERS:
        if shutil.which(cc):
            return cc
    return None


def check_cxx(root: Path, cxx: str) -> list[str]:
    """Compile every include/alp header as C++17 (syntax only)."""
    # Imported here, not at module level: the default (quick-profile) run of
    # this gate must stay hermetic and never shell out; only --cxx does.
    import subprocess

    inc = root / "include"
    offenders: list[str] = []
    for header in sorted((inc / "alp").rglob("*.h")):
        rel = header.relative_to(inc).as_posix()
        if rel in CXX_SKIP:
            continue
        with tempfile.TemporaryDirectory() as td:
            tu = Path(td) / "tu.cpp"
            tu.write_text("#include <" + rel + ">\n", encoding="utf-8")
            proc = subprocess.run(
                [
                    cxx,
                    "-std=c++17",
                    "-fsyntax-only",
                    "-DALP_BOARD_E1M_EVK",
                    f"-I{inc}",
                    str(tu),
                ],
                capture_output=True,
                text=True,
            )
        if proc.returncode != 0:
            first = next(
                (ln for ln in proc.stderr.splitlines() if "error" in ln),
                "compile failed",
            )
            offenders.append(
                f"include/{rel}: does not compile as C++17 ({first.strip()})"
            )
    return offenders


def check(root: Path) -> list[str]:
    alp_root = root / "include" / "alp"
    offenders: list[str] = []
    if not alp_root.is_dir():
        return offenders
    # Top-level only: sorted(alp_root.glob("*.h")) excludes ext/ and chips/.
    for header in sorted(alp_root.glob("*.h")):
        if header.name in ALLOWLIST:
            continue
        text = header.read_text(encoding="utf-8", errors="replace")
        for m in CHIP_INCLUDE_RE.finditer(text):
            line = text.count("\n", 0, m.start()) + 1
            offenders.append(
                f"include/alp/{header.name}:{line}: root public header includes "
                f"a chip surface ({m.group(0).strip()}); move the chip-specific "
                f"declaration behind include/alp/ext/<companion>/*.h"
            )
    return offenders


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    ap.add_argument(
        "--require-cxx", action="store_true", help="fail when no C++ compiler is found"
    )
    ap.add_argument(
        "--cxx", action="store_true", help="also compile every header as C++17"
    )
    args = ap.parse_args()

    offenders = check(args.root)
    if args.cxx:
        cxx = find_cxx()
        if cxx is not None:
            offenders += check_cxx(args.root, cxx)
        elif args.require_cxx:
            offenders.append("no C++ compiler found for the C++ purity check")
        else:
            print(
                "check_public_header_purity: no C++ compiler on PATH; C++ check skipped"
            )
    if not offenders:
        return 0

    print(
        "check_public_header_purity: public headers must stay chip-neutral and C++-clean:"
    )
    for o in offenders:
        print(f"  {o}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
