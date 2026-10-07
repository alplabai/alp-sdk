#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Reject a placeholder-only ALP-Bxxx diagnostic landing page (issue #1207).

metadata/error-catalog.json exports every docs/diagnostics/ALP-B*.md page as
the `doc:` destination for a stable diagnostic code -- `tan explain --code`,
the IDE, and CI all link a coded failure straight to that file
(scripts/gen_error_catalog.py globs the same docs/diagnostics/ALP-B*.md set
the catalog is built from, so "referenced by the catalog" and "exists under
docs/diagnostics/" are the same set by construction).  A page that still
reads

    (Diagnostic landing page -- narrative to be added.)

sends a user from a coded failure to an empty page instead of a diagnosis.

This gate, like scripts/gen_error_catalog.py, walks the docs/diagnostics/
ALP-B*.md page set -- it does not walk the scripts/alp_cli/validator.py
`code="ALP-B..."` emission sites, so a code that validator.py starts
emitting without ever getting a page would stay invisible to this gate.
scripts/check_diagnostic_emission_coverage.py closes that gap by walking
the emission sites and asserting each has a page (#1569).

This gate fails when a docs/diagnostics/ALP-B*.md page:

  1. still contains the stock placeholder sentence, or
  2. carries no `## Fix` / `## Resolution` / `## Remedy` section -- the one
     structural element every real landing page must have (a diagnosis with
     no remediation is still not useful), matching the section names
     scripts/gen_error_catalog.py itself lifts into the catalog's `fix` field.

Run locally:

    python3 scripts/check_diagnostic_narratives.py
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

_PLACEHOLDER_RE = re.compile(r"narrative to be added", re.IGNORECASE)
_FIX_SECTION_RE = re.compile(r"^#{2,}\s*(fix|resolution|remedy)\b", re.IGNORECASE | re.MULTILINE)


def find_problems(root: Path) -> list[str]:
    """Return one message per placeholder-only page under root/docs/diagnostics."""
    diag_dir = root / "docs" / "diagnostics"
    problems: list[str] = []
    for path in sorted(diag_dir.glob("ALP-B*.md")):
        rel = path.relative_to(root).as_posix()
        text = path.read_text(encoding="utf-8")
        if _PLACEHOLDER_RE.search(text):
            problems.append(
                f"{rel}: placeholder-only diagnostic page (still says "
                f"\"narrative to be added\") -- write the invariant, likely "
                f"causes, read-only diagnostics, fix, and escalation "
                f"guidance (see docs/diagnostics/ALP-B007.md for the shape)"
            )
            continue
        if not _FIX_SECTION_RE.search(text):
            problems.append(
                f"{rel}: no '## Fix' (or Resolution/Remedy) section -- every "
                f"diagnostic landing page must state the remediation"
            )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO,
                     help="Repo root to scan (default: this checkout).")
    args = ap.parse_args()

    problems = find_problems(args.root)
    if problems:
        print("check_diagnostic_narratives: FAIL", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        return 1

    print("OK   docs/diagnostics/ALP-B*.md  (no placeholder-only pages)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
