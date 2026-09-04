#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: no dead relative-path links in customer-facing documentation.

A Markdown link `[text](path)` whose target is a relative filesystem path
(not http(s)://, not mailto:, not a same-file `#anchor`) must resolve to a
real file -- otherwise a reader who clicks it, or a tool that follows it,
lands nowhere.  Issue #1213 ("existing green gates miss ... dead paths"):
no gate previously checked this, so a doc-to-doc or doc-to-code
cross-reference can go stale (the referenced file renamed or removed)
while every doc-drift / link-adjacent check stays green.

Scanned surface -- broader than check_doc_drift.py's dead-symbol check
(see its doc_files_for_symbol_scan()): a dead SYMBOL check must stay off
docs/adr/** and docs/abi/** because a historical decision record or ABI
ledger entry legitimately references a symbol that was later renamed or
removed *by design* -- that's not true of a filesystem PATH, so this gate
covers more ground:

    README.md, VERSIONS.md, docs/*.md (top-level only), docs/tutorials/**,
    docs/soms/**, docs/boards/**, docs/diagnostics/**, docs/adr/**,
    docs/abi/**, docs/ci/**, docs/bench/**, examples/**/README.md

Deliberately NOT scanned: docs/superpowers/** (living/draft design docs,
excluded from the doc-drift gate for the same reason) and CHANGELOG.md
(append-only history; a stale link in an old entry documents what was
true then, not a live cross-reference).

Deliberately NOT checked:
  * http(s):// / mailto: targets -- external-link validation is a
    different, network-dependent problem.
  * Same-file `#anchor` targets -- GitHub's heading-to-slug algorithm has
    enough edge cases (code spans, punctuation, explicit `{#id}`
    attributes some pages already use) that a naive re-implementation
    would produce false positives; left as future work.
  * Anything inside a fenced ``` or ~~~ code block, indented or not -- a
    snippet illustrating Markdown syntax is not a real link.

Path existence is checked case-sensitively on every platform (walking the
real on-disk directory listing, not `Path.exists()`/`Path.resolve()`,
which are case-insensitive on the Windows dev host this gate is written
to run local-first on) -- a wrong-case link would otherwise pass locally
and only 404 on the case-sensitive ubuntu-latest CI runner.

Suppression: add a (file, exact link-target text) pair to ALLOWLIST with a
one-line reason.  Reserve it for a genuine pre-existing break tracked
elsewhere -- a growing allowlist usually means the gate is catching a real
dead link that belongs fixed in the doc instead.

Run locally:

    python3 scripts/check_doc_links.py
"""
from __future__ import annotations

import argparse
import os.path
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# docs/ subdirectories recursed into for link-checking.  Wider than
# check_doc_drift.py's own _DOC_SUBDIRS (tutorials/soms/boards/bench only) --
# see the module docstring for why a path check safely covers adr/abi/
# diagnostics/ci where a symbol-drift check cannot.
_DOC_SUBDIRS = ("tutorials", "soms", "boards", "diagnostics", "adr", "abi", "ci", "bench")

_LINK_RE = re.compile(r"\[[^\]]*\]\(([^)]+)\)")
_FENCE_RE = re.compile(r"^\s*(```|~~~)")
_EXTERNAL_PREFIXES = ("http://", "https://", "mailto:")

# (file relative to repo root, exact link-target text) -> reason.
ALLOWLIST: dict[tuple[str, str], str] = {}


def _doc_files(root: Path) -> list[Path]:
    """Customer-facing docs, plus every example README -- see the module
    docstring for the exact scanned surface."""
    out: list[Path] = []
    readme = root / "README.md"
    if readme.is_file():
        out.append(readme)
    versions = root / "VERSIONS.md"
    if versions.is_file():
        out.append(versions)
    docs = root / "docs"
    if docs.is_dir():
        out.extend(sorted(docs.glob("*.md")))  # top-level only
        for sub in _DOC_SUBDIRS:
            d = docs / sub
            if d.is_dir():
                out.extend(sorted(d.rglob("*.md")))
    examples = root / "examples"
    if examples.is_dir():
        out.extend(sorted(examples.rglob("README.md")))
    return out


def _lexical_join(base: Path, path_part: str) -> Path:
    """Combine `base` and `path_part`, collapsing `.`/`..` purely by
    string manipulation (`os.path.normpath`) -- never `Path.resolve()`,
    which on Windows silently case-normalizes an EXISTING path to its
    real on-disk spelling before `_exists_case_sensitive` ever sees it,
    hiding the exact wrong-case link this check exists to catch."""
    return Path(os.path.normpath(str(base / path_part)))


def _exists_case_sensitive(path: Path) -> bool:
    """`Path.exists()` is case-insensitive on the Windows dev host this
    gate is written to run local-first on, but case-sensitive on the
    ubuntu-latest CI runner it also gates -- a link whose written case
    doesn't match the real filename can still 404 in Actions while
    passing locally.  Walk every path segment against the real on-disk
    directory listing (not `Path.resolve()`, which would silently
    correct the case before we could see the mismatch)."""
    try:
        parts = path.parts
    except ValueError:
        return path.exists()
    if not parts:
        return False
    cur = Path(parts[0])
    for part in parts[1:]:
        try:
            entries = {p.name for p in cur.iterdir()}
        except OSError:
            return False
        if part not in entries:
            return False
        cur = cur / part
    return True


def find_problems(root: Path) -> list[str]:
    """Return one message per dead relative-path link target."""
    problems: list[str] = []
    for doc in _doc_files(root):
        rel = doc.relative_to(root).as_posix()
        text = doc.read_text(encoding="utf-8", errors="replace")
        in_fence = False
        for lineno, line in enumerate(text.splitlines(), start=1):
            if _FENCE_RE.match(line):
                in_fence = not in_fence
                continue
            if in_fence:
                continue
            for m in _LINK_RE.finditer(line):
                target = m.group(1).strip()
                if target.startswith(_EXTERNAL_PREFIXES):
                    continue
                path_part = target.split("#", 1)[0]
                if not path_part:
                    continue  # pure same-file anchor -- not checked here
                if (rel, target) in ALLOWLIST:
                    continue
                if path_part.startswith("/"):
                    resolved = _lexical_join(root, path_part.lstrip("/"))
                else:
                    resolved = _lexical_join(doc.parent, path_part)
                if not _exists_case_sensitive(resolved):
                    problems.append(
                        f"{rel}:{lineno}: dead link -> '{target}' "
                        f"(resolves to {resolved}, which does not exist)"
                    )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO,
                     help="Repo root to scan (default: this checkout).")
    args = ap.parse_args()

    problems = find_problems(args.root)
    if problems:
        print("check_doc_links: FAIL", file=sys.stderr)
        for p in problems:
            print(f"  - {p}", file=sys.stderr)
        print(
            "\nFix the target path, or if this is a genuine pre-existing "
            "break already tracked elsewhere, add (file, target) to "
            "ALLOWLIST with a one-line reason.",
            file=sys.stderr,
        )
        return 1

    print("OK   customer docs (README.md, docs/*.md, "
          "docs/{tutorials,soms,boards,diagnostics,adr,abi,ci,bench}/**, "
          "examples/**/README.md) carry no dead relative-path links")
    return 0


if __name__ == "__main__":
    sys.exit(main())
