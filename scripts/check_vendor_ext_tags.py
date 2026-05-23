#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: every function declaration under include/alp/ext/**/*.h whose
name matches alp_<vendor>_<class>_<verb> must carry an
@par Supported silicon: tag in its immediately-preceding Doxygen block.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FUNC_RE = re.compile(
    r"^\s*[A-Za-z_][\w\s\*]*?\s+(alp_[a-z0-9]+_[a-z0-9]+_[a-z0-9_]+)\s*\(",
    re.MULTILINE,
)
TAG_RE = re.compile(r"@par\s+Supported\s+silicon\s*:", re.IGNORECASE)


def _check_header(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    issues: list[str] = []
    for m in FUNC_RE.finditer(text):
        fname = m.group(1)
        # Look back up to 2000 chars for an @par Supported silicon: tag.
        prefix_end = m.start()
        prefix = text[max(0, prefix_end - 2000):prefix_end]
        if not TAG_RE.search(prefix):
            issues.append(f"{path}:{fname}: missing '@par Supported silicon:' tag")
    return issues


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    ext_root = args.root / "include" / "alp" / "ext"
    if not ext_root.is_dir():
        return 0

    offenders: list[str] = []
    for h in sorted(ext_root.rglob("*.h")):
        offenders.extend(_check_header(h))

    if not offenders:
        return 0

    print("check_vendor_ext_tags: the following functions lack the silicon tag:")
    for o in offenders:
        print(f"  {o}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
