#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: every src/backends/**/sw_fallback.c carries both
@par Cost: and @par Performance: tags near the top of the file.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
COST_RE = re.compile(r"@par\s+Cost\s*:", re.IGNORECASE)
PERF_RE = re.compile(r"@par\s+Performance\s*:", re.IGNORECASE)


def _check_one(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    head = text[:2000]
    issues: list[str] = []
    if not COST_RE.search(head):
        issues.append(f"{path}: missing '@par Cost:' tag")
    if not PERF_RE.search(head):
        issues.append(f"{path}: missing '@par Performance:' tag")
    return issues


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    backends_root = args.root / "src" / "backends"
    if not backends_root.is_dir():
        return 0

    offenders: list[str] = []
    for f in sorted(backends_root.rglob("sw_fallback.c")):
        offenders.extend(_check_one(f))

    if not offenders:
        return 0

    print("check_sw_fallback_tags: the following SW fallbacks lack tags:")
    for o in offenders:
        print(f"  {o}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
