#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Assert every ALP-Bxxx code scripts/alp_cli/validator.py can emit has a
docs/diagnostics/ALP-Bxxx.md landing page (issue #1569).

scripts/check_diagnostic_narratives.py and scripts/gen_error_catalog.py both
walk the docs/diagnostics/ALP-B*.md page set -- neither walks the
scripts/alp_cli/validator.py `code="ALP-Bxxx"` emission sites. That means a
new/renamed code added to validator.py ships with no doc page and no catalog
entry, and nothing in CI flags it: exactly how ALP-B000 and ALP-B099 went
undocumented until #1511 caught it by inspection.

This gate greps validator.py for every `code="ALP-Bxxx"` emission and fails
if any emitted code has no docs/diagnostics/<code>.md page. It does not check
the reverse direction (a doc page with no emission site is not a bug -- a
code can be retired from validator.py while its landing page stays as
historical/reference documentation).

Run locally:

    python3 scripts/check_diagnostic_emission_coverage.py
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

_EMIT_RE = re.compile(r'code="(ALP-B\d+)"')


def find_problems(root: Path) -> list[str]:
    """Return one message per emitted ALP-Bxxx code with no doc page."""
    validator = root / "scripts" / "alp_cli" / "validator.py"
    if not validator.exists():
        return [f"{validator.relative_to(root).as_posix()}: file not found"]

    text = validator.read_text(encoding="utf-8")
    emitted = sorted(set(_EMIT_RE.findall(text)))

    diag_dir = root / "docs" / "diagnostics"
    problems: list[str] = []
    for code in emitted:
        doc = diag_dir / f"{code}.md"
        if not doc.exists():
            problems.append(
                f"scripts/alp_cli/validator.py: emits code=\"{code}\" but "
                f"{doc.relative_to(root).as_posix()} does not exist -- add "
                f"the landing page (see docs/diagnostics/ALP-B007.md for the "
                f"shape) and rerun scripts/gen_error_catalog.py"
            )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO,
                     help="Repo root to scan (default: this checkout).")
    args = ap.parse_args()

    problems = find_problems(args.root)
    if problems:
        print("check_diagnostic_emission_coverage: FAIL", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    print("OK   scripts/alp_cli/validator.py ALP-Bxxx emission sites  (every code has a doc page)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
