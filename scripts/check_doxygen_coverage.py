#!/usr/bin/env python3
# Copyright 2026 ALP Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Doxygen-coverage audit -- walks include/alp/*.h and flags every
public function declaration that lacks a `@brief` (or `\\brief`) tag
in the preceding doc-comment block.

The pr-doxygen.yml workflow now runs Doxygen with WARN_AS_ERROR=YES
(FAIL_ON_WARNINGS gate, enabled since commit 718d81d).  Every public
function therefore needs a `@brief` or the CI Doxygen build fails.
This script provides incremental per-function gap detection that
catches missing tags BEFORE the Doxygen gate fails — useful for
driving coverage to zero without needing Doxygen installed locally.

Heuristic:
  * A function declaration looks like `<type> name(...);` on a line
    by itself in a header file.
  * The block immediately preceding it (`/** ... */` or `///`-style)
    counts as the doc-comment.
  * If that block doesn't contain `@brief` or `\\brief`, flag it.
  * If there's no preceding block at all, also flag it.

Run from the repo root:

    python3 scripts/check_doxygen_coverage.py         # informational
    python3 scripts/check_doxygen_coverage.py --fail-on-gaps   # CI
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from typing import Optional


ROOT         = pathlib.Path(__file__).resolve().parent.parent
INCLUDE_ROOT = ROOT / "include" / "alp"


def public_functions_with_context(text: str) -> list[tuple[int, str, str]]:
    """Return [(line_no, function_name, preceding_doc_block)] for each
    public function declaration in `text`."""
    # Strip line comments first; they don't affect declarations.
    # Keep block comments because we'll inspect them for `@brief`.
    out: list[tuple[int, str, str]] = []
    lines = text.splitlines()

    # Pre-pass: find block-comment ranges so we can map any line to
    # "the comment block ending at the previous non-comment line".
    in_block = False
    block_start = -1
    blocks: list[tuple[int, int, str]] = []  # (start_line, end_line, text)
    block_text: list[str] = []
    for i, line in enumerate(lines):
        if not in_block and "/*" in line:
            in_block = True
            block_start = i
            block_text = [line]
            if "*/" in line and line.index("*/") > line.index("/*"):
                in_block = False
                blocks.append((block_start, i, "\n".join(block_text)))
                block_text = []
        elif in_block:
            block_text.append(line)
            if "*/" in line:
                in_block = False
                blocks.append((block_start, i, "\n".join(block_text)))
                block_text = []

    def preceding_block(line_no: int) -> Optional[str]:
        """Return the last block whose end_line is one of the immediate
        non-blank predecessors of line_no."""
        cursor = line_no - 1
        while cursor >= 0 and lines[cursor].strip() == "":
            cursor -= 1
        for (start, end, body) in reversed(blocks):
            if end == cursor:
                return body
        return None

    # Function-decl regex: same shape as check_test_coverage.py.
    # Negative-lookahead `typedef` skips function-pointer typedef lines
    # like `typedef void (*foo_t)(int);` which would otherwise match
    # with name = "void".
    func_decl = re.compile(
        r"^\s*(?!\s*static\b)(?!\s*#)(?!\s*typedef\b)"
        r"(?:[A-Za-z_][\w\s\*\,\(\)]*?)\s+"
        r"([a-z][a-z0-9_]+)\s*\([^;{]*\)\s*;\s*$"
    )

    # Identifiers that look like function names to the regex but are
    # actually C type keywords picked up when the return type wraps.
    _C_KEYWORDS = {
        "void", "int", "char", "short", "long", "float", "double",
        "unsigned", "signed", "size_t", "ssize_t", "bool", "extern",
    }

    for i, line in enumerate(lines):
        m = func_decl.match(line)
        if not m:
            continue
        name = m.group(1)
        if name in _C_KEYWORDS:
            continue
        if name.endswith("_t"):
            continue
        block = preceding_block(i)
        out.append((i + 1, name, block or ""))
    return out


def _block_has_brief(block: str) -> bool:
    """Return True iff `block` (a `/** ... */` body) gives Doxygen a
    brief description.

    Two ways to qualify:
      1. Explicit `@brief` / `\\brief` tag anywhere in the block.
      2. Doxygen's `JAVADOC_AUTOBRIEF = YES` mode (set in
         .github/workflows/pr-doxygen.yml) treats the first sentence
         of a doc-comment as @brief automatically.  So a block with
         non-empty body text and *no* `@`-tags at all also counts.
    """
    if not block.strip():
        return False
    if "@brief" in block or r"\brief" in block:
        return True
    # Auto-brief mode: strip block-comment markers + see if there's
    # any prose before any `@`-tag.
    stripped = re.sub(r"^\s*/?\*+/?", "", block, flags=re.MULTILINE).strip()
    if not stripped:
        return False
    # Find the first `@<tag>` -- if there's prose before it, that
    # prose is auto-brief.  Otherwise: missing.
    m = re.search(r"@\w+", stripped)
    if m is None:
        return True  # entire block is prose -> auto-brief
    return m.start() > 0


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fail-on-gaps", action="store_true",
                        help="exit 1 if any public function is missing @brief")
    parser.add_argument("--verbose", action="store_true",
                        help="list every covered + uncovered function")
    args = parser.parse_args(argv)

    total = 0
    covered = 0
    gaps: list[tuple[str, int, str]] = []

    for header in sorted(INCLUDE_ROOT.rglob("*.h")):
        text = header.read_text(encoding="utf-8", errors="replace")
        rel = header.relative_to(INCLUDE_ROOT.parent).as_posix()
        for line_no, name, block in public_functions_with_context(text):
            total += 1
            has_brief = _block_has_brief(block)
            if has_brief:
                covered += 1
                if args.verbose:
                    print(f"OK    {rel}:{line_no}  {name}")
            else:
                gaps.append((rel, line_no, name))

    print()
    print("Doxygen @brief coverage:")
    print(f"  Total public functions: {total}")
    if total:
        print(f"  With @brief:            {covered}  ({100*covered/total:.1f}%)")
    print(f"  Missing @brief:         {len(gaps)}")

    if gaps:
        print()
        print("Functions missing @brief in their preceding doc-comment:")
        for rel, line_no, name in gaps:
            print(f"  {rel}:{line_no}  {name}")

    if args.fail_on_gaps and gaps:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
