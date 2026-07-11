#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: chip-header `@par Driver status:` labels match metadata truth.

Driver readiness has a single source of truth in
``metadata/chips/<id>.yaml`` (``driver_status:``).  Chip headers under
``include/alp/chips/<id>.h`` carry a human-readable ``@par Driver status:``
line for the generated Doxygen, and those labels had drifted from the
metadata (issue #501: e.g. ``sh1106.h`` said ``[stub-impl]`` while the
metadata said ``complete``).  This gate keeps the header label's *status
word* in lockstep with the metadata so generated docs never misreport what
is implemented.

Only the status word is checked; the free-text description after it (what
exactly is implemented) is not constrained.  Accepted header spellings:
``[stub-impl]`` / ``[partial-impl]`` / ``[complete-impl]`` / ``[planned-impl]``
and the bare ``STUB`` / ``PARTIAL`` / ``COMPLETE`` / ``PLANNED`` forms.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

try:
    import yaml  # type: ignore[import-untyped]
except ImportError:
    sys.exit("check_chip_header_status: PyYAML is required.")

VALID = {"stub", "partial", "complete", "planned"}
STATUS_RE = re.compile(r"@par\s+Driver status:\s*([^\n—-]+)")


def _norm_label(raw: str) -> str | None:
    """Header label -> canonical status word, or None if unrecognised."""
    s = raw.strip().strip("[]").lower().strip()
    return s if s in VALID else None


def check(root: Path) -> list[str]:
    headers = root / "include" / "alp" / "chips"
    meta = root / "metadata" / "chips"
    issues: list[str] = []
    if not headers.is_dir() or not meta.is_dir():
        return issues
    for header in sorted(headers.glob("*.h")):
        meta_file = meta / f"{header.stem}.yaml"
        if not meta_file.is_file():
            continue
        m = STATUS_RE.search(header.read_text(encoding="utf-8", errors="replace"))
        if m is None:
            continue  # the header tag is optional; validate it only when present
        doc = yaml.safe_load(meta_file.read_text(encoding="utf-8")) or {}
        meta_status = doc.get("driver_status")
        if meta_status not in VALID:
            continue  # metadata-vocabulary parity is a separate gate
        hdr_status = _norm_label(m.group(1))
        if hdr_status is None:
            issues.append(
                f"include/alp/chips/{header.name}: unrecognised "
                f"'@par Driver status:' word '{m.group(1).strip()}' "
                f"(expected one of {sorted(VALID)})"
            )
        elif hdr_status != meta_status:
            issues.append(
                f"include/alp/chips/{header.name}: header status "
                f"'{hdr_status}' != metadata driver_status '{meta_status}' "
                f"({meta_file.relative_to(root)})"
            )
    return issues


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    issues = check(args.root)
    if not issues:
        return 0

    print("check_chip_header_status: chip-header driver-status labels drifted "
          "from metadata/chips/*.yaml:")
    for i in issues:
        print(f"  {i}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
