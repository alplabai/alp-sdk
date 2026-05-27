#!/usr/bin/env python3
# Copyright 2026 ALP Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Doc-drift gate -- fails (exit 1) when customer-facing documentation
references SDK identifiers that no longer exist, or when a top-level
doc isn't linked from the docs index.

Two independent checks:

  (a) Dead-symbol references.  Every `ALP_[A-Z0-9_]+` and
      `alp_[a-z0-9_]+` token mentioned in a customer doc must exist as
      a token somewhere under include/alp/**/*.h.  A token that appears
      in the docs but in NO public header is "dead" -- almost always a
      rename the docs missed (e.g. the DEEPX_DX -> DEEPX_DXM1 /
      .alpmodel migration left ALP_..._DEEPX_DX references behind).

      Scanned surfaces (customer-facing only):
        README.md, docs/*.md (top-level), docs/tutorials/**,
        docs/soms/**, docs/boards/**
      Deliberately NOT scanned (historical / generated / internal):
        CHANGELOG.md, docs/superpowers/**, docs/abi/**, docs/adr/**

  (b) Docs-index integrity.  Every top-level docs/*.md (except
      README.md itself) must be linked from docs/README.md.  This is
      the check the updating-docs skill documents in prose; here it is
      mechanised so CI enforces it.

Run from the repo root:

    python3 scripts/check_doc_drift.py                  # both checks
    python3 scripts/check_doc_drift.py --allow ALP_FOO  # extend allowlist

Exits non-zero if either check finds a problem.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import sys
from typing import Optional


ROOT = pathlib.Path(__file__).resolve().parent.parent

# Tokens that look like dead symbols but are legitimately absent from
# include/alp/**/*.h.  Keep this list SHORT and justify every entry --
# a growing allowlist usually means the gate is catching real drift
# that belongs fixed in the docs instead.
_ALLOWLIST: set[str] = set()
# (populated during the run-and-fix phase with justified entries)

# Identifier shapes we treat as SDK symbols.
_SYMBOL_RE = re.compile(r"\b(ALP_[A-Z0-9_]+|alp_[a-z0-9_]+)\b")

# docs/ subdirectories scanned recursively for dead symbols.
_DOC_SUBDIRS = ("tutorials", "soms", "boards")


def collect_header_symbols(include_root: pathlib.Path) -> set[str]:
    """Return every ALP_*/alp_* token appearing anywhere under
    include_root/**/*.h.  Existence in ANY header = the symbol is real."""
    symbols: set[str] = set()
    if not include_root.is_dir():
        return symbols
    for header in include_root.rglob("*.h"):
        text = header.read_text(encoding="utf-8", errors="replace")
        symbols.update(m.group(1) for m in _SYMBOL_RE.finditer(text))
    return symbols


def doc_files_for_symbol_scan(root: pathlib.Path) -> list[pathlib.Path]:
    """Customer-facing docs scanned for dead symbols (spec scope)."""
    out: list[pathlib.Path] = []
    readme = root / "README.md"
    if readme.is_file():
        out.append(readme)
    docs = root / "docs"
    if docs.is_dir():
        out.extend(sorted(docs.glob("*.md")))           # top-level only
        for sub in _DOC_SUBDIRS:
            d = docs / sub
            if d.is_dir():
                out.extend(sorted(d.rglob("*.md")))      # recursive
    return out


def _is_known(tok: str, known: set[str], allow: set[str]) -> bool:
    """A token is real if it is a known header symbol, allowlisted, or --
    for trailing-underscore family/wildcard references like `ALP_E1M_*`
    (token captured as `ALP_E1M_`) -- a prefix of some real symbol."""
    if tok in known or tok in allow:
        return True
    if tok.endswith("_"):
        return any(s.startswith(tok) for s in known)
    return False


def find_dead_symbols(root: pathlib.Path, known: set[str],
                      allow: set[str]) -> list[tuple[str, int, str]]:
    """Return [(relpath, line_no, token)] for every dead symbol ref."""
    dead: list[tuple[str, int, str]] = []
    for doc in doc_files_for_symbol_scan(root):
        rel = doc.relative_to(root).as_posix()
        text = doc.read_text(encoding="utf-8", errors="replace")
        for line_no, line in enumerate(text.splitlines(), 1):
            for m in _SYMBOL_RE.finditer(line):
                tok = m.group(1)
                if not _is_known(tok, known, allow):
                    dead.append((rel, line_no, tok))
    return dead


def find_index_gaps(root: pathlib.Path) -> list[str]:
    """Return top-level docs/*.md filenames not linked from docs/README.md."""
    docs = root / "docs"
    index = docs / "README.md"
    if not index.is_file():
        return []
    linked = set(re.findall(r"[A-Za-z0-9_-]+\.md",
                            index.read_text(encoding="utf-8")))
    gaps: list[str] = []
    for md in sorted(docs.glob("*.md")):
        if md.name == "README.md":
            continue
        if md.name not in linked:
            gaps.append(md.name)
    return gaps


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--root", default=str(ROOT),
                        help="repo root to scan (default: the alp-sdk checkout)")
    parser.add_argument("--allow", action="append", default=[], metavar="SYMBOL",
                        help="extend the dead-symbol allowlist (repeatable)")
    args = parser.parse_args(argv)

    root = pathlib.Path(args.root).resolve()
    allow = _ALLOWLIST | set(args.allow)

    known = collect_header_symbols(root / "include" / "alp")
    dead = find_dead_symbols(root, known, allow)
    gaps = find_index_gaps(root)

    if dead:
        print("Dead SDK-symbol references "
              "(token not found in include/alp/**/*.h):", file=sys.stderr)
        for rel, line_no, tok in dead:
            print(f"  {rel}:{line_no}  {tok}", file=sys.stderr)
    if gaps:
        print("Top-level docs/*.md not linked from docs/README.md:",
              file=sys.stderr)
        for name in gaps:
            print(f"  docs/{name}", file=sys.stderr)

    if dead or gaps:
        print(f"\ndoc-drift: {len(dead)} dead ref(s), {len(gaps)} index "
              f"gap(s) -- failing.", file=sys.stderr)
        return 1

    print("doc-drift: OK (no dead symbol refs, docs index complete).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
