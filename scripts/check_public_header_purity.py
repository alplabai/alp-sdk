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
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

CHIP_INCLUDE_RE = re.compile(r'^\s*#\s*include\s*[<"]alp/chips/[^">]+[">]', re.MULTILINE)

# Root headers explicitly permitted to re-export a chip surface, each with a
# justification.  Keep this list empty unless a documented protocol/extension
# surface genuinely must live at the root; prefer include/alp/ext/ instead.
ALLOWLIST: dict[str, str] = {}


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
    args = ap.parse_args()

    offenders = check(args.root)
    if not offenders:
        return 0

    print("check_public_header_purity: root public headers must stay chip-neutral:")
    for o in offenders:
        print(f"  {o}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
