# SPDX-License-Identifier: Apache-2.0
"""Pure board.yaml migration engine (epic #610 WS6-b).

Comment/order-preserving via ruamel round-trip. No IO beyond the caller's;
`load`/`dump` are string<->doc, everything else operates on the ruamel doc.

Read-path leniency (absent schemaVersion == v1) lives in the resolver, NOT
here: the engine must tell "absent" from "explicit 1" to know a file still
needs the adoption stamp.
"""
from __future__ import annotations

import difflib
import io
from dataclasses import dataclass, field
from typing import Any

from ruamel.yaml import YAML

from .migrations import STEPS

LATEST: int = max(to for _from, to, _fn in STEPS)


class MigrateError(Exception):
    """Un-migratable input (unknown/newer version, malformed doc)."""


@dataclass
class Report:
    steps: list[str] = field(default_factory=list)
    needs_manual: list[tuple[str, str]] = field(default_factory=list)


def _yaml() -> YAML:
    y = YAML()
    y.preserve_quotes = True
    y.width = 4096  # don't rewrap long lines (e.g. pins: [...] flow entries)
    y.indent(mapping=2, sequence=4, offset=2)  # match repo board.yaml style
    return y


def load(text: str) -> Any:
    return _yaml().load(text)


def dump(doc: Any) -> str:
    buf = io.StringIO()
    _yaml().dump(doc, buf)
    return buf.getvalue()


def current_version(doc: Any) -> int | None:
    """Explicit schemaVersion, or None when absent."""
    v = doc.get("schemaVersion") if hasattr(doc, "get") else None
    return int(v) if v is not None else None


def plan(doc: Any) -> list[tuple[int | None, int]]:
    """Ordered (from, to) steps to reach canonical LATEST; [] when canonical.

    Chains from the doc's current version through each registry step:
    None -> adoption (1), then each k -> k+1. `running` tracks the version
    after the steps selected so far, so a brand-new unstamped file collects
    the whole chain (None->1->2->...) in one pass.
    """
    cur = current_version(doc)
    if cur is not None and cur > LATEST:
        raise MigrateError(
            f"board.yaml schemaVersion {cur} is newer than this SDK's "
            f"latest ({LATEST}); refusing to downgrade")
    running = cur  # None or int; STEPS is ordered by `to` ascending
    out: list[tuple[int | None, int]] = []
    for frm, to, _fn in STEPS:
        if frm == running:  # None == None (adoption) or int == int (bump)
            out.append((frm, to))
            running = to
    return out


def apply(doc: Any) -> tuple[Any, Report]:
    """Run every planned step in order; return the mutated doc + Report."""
    report = Report()
    wanted = set(plan(doc))
    for frm, to, fn in STEPS:
        if (frm, to) in wanted:
            fn(doc, report)
    return doc, report


def _leading_comment_end(lines: list[str]) -> int:
    """Index of the first line that is neither blank nor a whole-line comment
    -- i.e. where the top-level mapping starts. A file-banner comment block
    therefore stays on top; schemaVersion is inserted as the first real key
    below it."""
    for i, ln in enumerate(lines):
        s = ln.strip()
        if s and not s.startswith("#"):
            return i
    return len(lines)


def apply_text(text: str) -> tuple[str, Report]:
    """Byte-faithful migration: returns text with only the migration's line
    edits applied, everything else (comments, flow style, indentation)
    preserved exactly. Parsing/planning still go through the round-trip doc.

    Only the additive adoption step (#001) exists in this slice; a future
    structural step must extend this with its own text strategy or an
    explicit round-trip fallback.
    """
    doc = load(text)
    report = Report()
    steps = plan(doc)  # raises MigrateError on downgrade
    if not steps:
        return text, report
    lines = text.splitlines(keepends=True)
    for frm, to in steps:
        if frm is None and to == 1:
            idx = _leading_comment_end(lines)
            lines.insert(idx, f"schemaVersion: {to}\n")
            report.steps.append("m000_to_v1: stamped schemaVersion: 1")
        else:
            raise MigrateError(f"no text writer for migration step {frm}->{to}")
    return "".join(lines), report


def diff(old_text: str, new_text: str, path: str) -> str:
    return "".join(difflib.unified_diff(
        old_text.splitlines(keepends=True),
        new_text.splitlines(keepends=True),
        fromfile=f"a/{path}", tofile=f"b/{path}"))


def report_to_diagnostics(report: Report, uri: str) -> dict:
    """Render a Report as a diagnostic-v1 JSON object."""
    diags = []
    for step in report.steps:
        diags.append({
            "range": {"start": {"line": 0, "character": 0},
                      "end": {"line": 0, "character": 0}},
            "severity": "info",
            "code": "alp.migrate.applied",
            "message": step,
        })
    for loc, msg in report.needs_manual:
        diags.append({
            "range": {"start": {"line": 0, "character": 0},
                      "end": {"line": 0, "character": 0}},
            "severity": "warning",
            "code": "alp.migrate.needs-manual",
            "message": f"{loc}: {msg}",
        })
    return {"schemaVersion": 1, "tool": "alp-migrate",
            "diagnostics": [{"uri": uri, **d} for d in diags]}
