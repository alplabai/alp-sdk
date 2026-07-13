# SPDX-License-Identifier: Apache-2.0
"""Pure board.yaml migration engine (epic #610 WS6-b).

Lazy versioning: a board.yaml with no `schemaVersion` key IS version 1 (the
floor) -- handwritten and external projects keep loading unchanged and are
never "drift". The key only ever appears in a file once a migration has
actually bumped it to v2+. There is no adoption/stamp step; `LATEST` is 1 and
the migration registry is empty until the first real schema change lands.

Migrations are byte-faithful text transforms (comments, flow style, and
indentation are preserved because the file body is never re-serialized). Each
registry step is `(FROM, TO, apply_text_fn)` where `apply_text_fn(lines,
report)` mutates the file's lines in place -- including writing its own
`schemaVersion: TO` bump (use the `set_schema_version` helper). Parsing (for
version reads + planning) is a plain PyYAML load; the write path never
re-serializes the body, so no round-trip/comment-preserving loader -- and no
extra dependency -- is needed.
"""
from __future__ import annotations

import difflib
from dataclasses import dataclass, field
from typing import Any, Callable

import yaml  # PyYAML -- already a repo dep; only used to READ schemaVersion

from .migrations import STEPS

# The current board.yaml schema version. Bump this in lockstep with adding a
# registry STEP whose TO is higher. Defined explicitly (not derived from
# STEPS) so it stays 1 while the registry is empty.
LATEST: int = 1

# A migration step's text transform: mutate `lines` (a keepends splitlines
# list) in place and append a human note to `report.steps`.
TextStep = Callable[[list, "Report"], None]


class MigrateError(Exception):
    """Un-migratable input (a schemaVersion newer than this SDK's LATEST)."""


@dataclass
class Report:
    steps: list[str] = field(default_factory=list)
    needs_manual: list[tuple[str, str]] = field(default_factory=list)


def load(text: str) -> Any:
    """Parse board.yaml text to a dict for version reads only.

    The migration WRITE path is text-based (`apply_text`) and never
    re-serializes the body, so a plain PyYAML parse is enough here -- no
    round-trip / comment-preserving loader (and no extra dependency) is needed
    just to read `schemaVersion` and plan steps."""
    return yaml.safe_load(text)


def current_version(doc: Any) -> int:
    """The board.yaml's schema version. Absent `schemaVersion` == 1 (floor)."""
    v = doc.get("schemaVersion") if hasattr(doc, "get") else None
    return int(v) if v is not None else 1


def plan(doc: Any) -> list[tuple[int, int]]:
    """Ordered (from, to) steps to reach LATEST; [] when already current.

    Chains from the doc's current version through the registry: each k -> k+1.
    `running` tracks the version after the steps selected so far.
    """
    cur = current_version(doc)
    if cur > LATEST:
        raise MigrateError(
            f"board.yaml schemaVersion {cur} is newer than this SDK's "
            f"latest ({LATEST}); refusing to downgrade")
    running = cur
    out: list[tuple[int, int]] = []
    for frm, to, _fn in STEPS:  # STEPS ordered by `to` ascending
        if frm == running:
            out.append((frm, to))
            running = to
    return out


def _leading_comment_end(lines: list[str]) -> int:
    """Index of the first line that is neither blank nor a whole-line comment
    -- i.e. where the top-level mapping starts. A file-banner comment block
    therefore stays on top; an inserted key lands as the first real key."""
    for i, ln in enumerate(lines):
        s = ln.strip()
        if s and not s.startswith("#"):
            return i
    return len(lines)


def set_schema_version(lines: list[str], version: int) -> None:
    """Insert or update the top-level `schemaVersion:` line in place.

    Migration text-transforms call this to record their target version. If a
    `schemaVersion:` line already exists it is rewritten; otherwise the key is
    inserted as the first real key (below any leading banner comment)."""
    for i, ln in enumerate(lines):
        if ln.lstrip().startswith("schemaVersion:") and not ln.lstrip().startswith("#"):
            eol = "\n" if ln.endswith("\n") else ""
            lines[i] = f"schemaVersion: {version}{eol}"
            return
    idx = _leading_comment_end(lines)
    lines.insert(idx, f"schemaVersion: {version}\n")


def apply_text(text: str) -> tuple[str, Report]:
    """Apply every planned migration step to `text`, byte-faithfully.

    Returns the migrated text + a Report. With an empty registry (no schema
    change has landed yet) this is always a no-op that returns `text`
    unchanged -- nothing gets stamped until a real migration exists.
    """
    report = Report()
    planned = set(plan(load(text)))  # raises MigrateError on a newer version
    if not planned:
        return text, report
    lines = text.splitlines(keepends=True)
    for frm, to, fn in STEPS:
        if (frm, to) in planned:
            fn(lines, report)
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
