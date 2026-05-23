#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: every src/backends/**/*_stub.c file must reference an open
GitHub issue in its top comment block, on a line of the form:

    @par Tracking: github.com/alplabai/alp-sdk/issues/<N>

Exits 0 when every stub file is compliant (or no stub files exist).
Exits 1 listing the offenders otherwise.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ISSUE_RE = re.compile(
    r"@par\s+Tracking:\s*github\.com/alplabai/alp-sdk/issues/\d+",
    re.IGNORECASE,
)


def _check_one(path: Path) -> str | None:
    text = path.read_text(encoding="utf-8", errors="replace")
    head = text[:2000]  # the tag should live near the top
    if ISSUE_RE.search(head):
        return None
    return f"{path}: missing '@par Tracking: github.com/alplabai/alp-sdk/issues/<N>' tag"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    backends_root = args.root / "src" / "backends"
    if not backends_root.is_dir():
        return 0  # no backends to check yet

    offenders: list[str] = []
    for stub in sorted(backends_root.rglob("*_stub.c")):
        err = _check_one(stub)
        if err:
            offenders.append(err)

    if not offenders:
        return 0

    print("check_stub_issues: the following stub backends lack an issue link:")
    for o in offenders:
        print(f"  {o}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
