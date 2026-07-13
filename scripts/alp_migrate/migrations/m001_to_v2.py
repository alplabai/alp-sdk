# SPDX-License-Identifier: Apache-2.0
"""board.yaml v1 -> v2: unify `libraries:` into one top-level list (epic #610 §6).

v1 declared curated libraries in two places::

    libraries: [<canonical-name>, ...]        # top-level, project-wide (ADR 0018)
    cores.<id>.libraries: [<legacy-token>, ...]   # per-core, scoped to one core

v2 collapses both into a single top-level list of `{name, cores?}` objects::

    libraries:
      - name: <canonical>              # project-wide (cores omitted)
      - name: <canonical>
        cores: [<id>]                  # scoped to the named core(s)

Every legacy per-core token is mapped to its canonical manifest name through
metadata/library-aliases-v1.json.  The transform is line-based and
byte-faithful: only the `libraries:` blocks are rewritten (plus the
`schemaVersion: 2` stamp); every other line -- comments, ordering, blank lines,
indentation -- is preserved verbatim.  A board.yaml with no libraries is only
version-stamped.
"""
from __future__ import annotations

import json
import re
from pathlib import Path

import yaml

import alp_migrate  # partial at import time; attributes used at run time only

_LIB_KEY = re.compile(r"^(\s*)libraries:\s*(.*)$")


def _alias() -> dict[str, str]:
    """Legacy token -> canonical manifest name (metadata/library-aliases-v1.json)."""
    root = Path(__file__).resolve().parents[3]
    path = root / "metadata" / "library-aliases-v1.json"
    if not path.is_file():
        return {}
    doc = json.loads(path.read_text(encoding="utf-8"))
    aliases = doc.get("aliases")
    return dict(aliases) if isinstance(aliases, dict) else {}


def _indent_of(line: str) -> int:
    return len(line) - len(line.lstrip(" "))


def _block_start(line: str):
    """(indent, rest) for a real `libraries:` key line, else None.

    `rest` is whatever follows the colon (empty for a block-style list, the
    flow text for `libraries: [a, b]`).  Comment lines never match."""
    if line.lstrip().startswith("#"):
        return None
    m = _LIB_KEY.match(line)
    if not m:
        return None
    return len(m.group(1)), m.group(2).strip()


def _library_block_indices(lines: list[str]) -> set[int]:
    """Indices of every line belonging to a `libraries:` block (key + items).

    For a top-level (indent 0) block, a single immediately-following blank line
    is swept too so removing a project-wide `libraries:` above `cores:` does not
    leave a dangling blank."""
    drop: set[int] = set()
    i = 0
    n = len(lines)
    while i < n:
        bs = _block_start(lines[i])
        if bs is None:
            i += 1
            continue
        indent, rest = bs
        drop.add(i)
        j = i + 1
        if not rest:  # block-style list: consume deeper-indented item lines
            while j < n:
                if lines[j].strip() == "":
                    break
                if _indent_of(lines[j]) <= indent:
                    break
                drop.add(j)
                j += 1
        if indent == 0 and j < n and lines[j].strip() == "":
            drop.add(j)
            j += 1
        i = j
    return drop


def _collect(text: str, alias: dict[str, str]):
    """(project_wide, per_core) canonical-name selections parsed from a v1 doc."""
    doc = yaml.safe_load(text) or {}
    project_wide: list[str] = []
    for tok in (doc.get("libraries") or []):
        if isinstance(tok, str):
            project_wide.append(alias.get(tok, tok))
    per_core: list[tuple[str, str]] = []
    for cid, centry in (doc.get("cores") or {}).items():
        if isinstance(centry, dict):
            for tok in (centry.get("libraries") or []):
                if isinstance(tok, str):
                    per_core.append((alias.get(tok, tok), str(cid)))
    return project_wide, per_core


def _render(project_wide: list[str],
            per_core: list[tuple[str, str]]) -> list[str]:
    """Render the unified top-level `libraries:` block (keepends lines)."""
    out = ["libraries:\n"]
    for name in project_wide:
        out.append(f"  - name: {name}\n")
    for name, cid in per_core:
        out.append(f"  - name: {name}\n")
        out.append(f"    cores: [{cid}]\n")
    return out


def apply(lines: list[str], report) -> None:
    """In-place v1 -> v2 transform (see module docstring)."""
    text = "".join(lines)
    alias = _alias()
    project_wide, per_core = _collect(text, alias)
    has_libs = bool(project_wide or per_core)

    if has_libs:
        drop = _library_block_indices(lines)
        kept = [ln for k, ln in enumerate(lines) if k not in drop]
        lines[:] = kept

    alp_migrate.set_schema_version(lines, 2)

    if has_libs:
        # Insert the unified block right after the schemaVersion stamp.
        for idx, ln in enumerate(lines):
            if ln.lstrip().startswith("schemaVersion:") \
                    and not ln.lstrip().startswith("#"):
                block = _render(project_wide, per_core) + ["\n"]
                lines[idx + 1:idx + 1] = block
                break

    n = len(project_wide) + len(per_core)
    report.steps.append(
        f"m001_to_v2: unified {n} library selection(s) into the top-level "
        f"`libraries:` list; stamped schemaVersion 2"
        if has_libs else "m001_to_v2: stamped schemaVersion 2 (no libraries)")
