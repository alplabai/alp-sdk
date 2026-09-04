#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""Flag a closed/merged GitHub issue cited as an open blocker in a
driver-status prose block (issue #1950).

WHY THIS EXISTS
---------------
On 2026-09-04 `metadata/chips/gd32g553.yaml`'s `driver_status: partial`
comment named #495, #494 and #496 as the open reasons the driver is not
`complete`. All three had closed months earlier -- #494/#495 via #730, #496
via #764 (fixed in #1949). The six chip-status gates
(`check_stub_issues.py`, `check_stub_symbol_matrix.py`,
`check_driver_status_backing.py`, `check_chip_header_status.py`,
`check_sw_fallback_tags.py`, `check_vendor_ext_tags.py`) all stayed green
throughout: they bind a status *word* to a source artefact, never the free
*prose* explaining it. A reader who follows a citation like that into a
closed issue either wrongly concludes the entry is done, or re-does
analysis the closing comment already contains -- worse than citing nothing.

WHAT IT CATCHES, AND WHAT IT DOES NOT -- read this before trusting it
--------------------------------------------------------------------
CATCHES:
  * a `#NNNN` citation inside a `<...>driver_status:` field's trailing YAML
    comment block (metadata/**/*.yaml), or inside an `@par Driver status:`
    Doxygen paragraph (include/alp/chips/*.h), whose number resolves in the
    snapshot to a state other than OPEN, UNLESS the citation reads as a
    historical reference (see HISTORICAL below).

DOES NOT CATCH, on purpose (scope, per #1950's own design discussion):
  * a citation anywhere else -- `changelog.d/`, `CHANGELOG.md`, ADR prose,
    `notes:` blocks, any other YAML field's comment. Those files are full of
    legitimate historical references ("fixed in #N") and a blanket `#NNNN`
    harvest over them is noise from day one. Widening the harvester there is
    left for a follow-up if it turns out to be cheap.
  * a citation whose surrounding clause reads as historical rather than a
    blocker claim (see HISTORICAL below) -- reported as neither pass nor
    fail; it is simply not examined.
  * anything when the issue-state snapshot is missing or stale: every
    finding degrades to a WARNING and the gate exits 0 (see STALENESS).

HISTORICAL -- distinguishing "blocked on #N" from "landed via #N"
-------------------------------------------------------------------
The whole point of scoping to a driver-status block is that the block is BY
CONSTRUCTION a claim about what remains -- but the block can still narrate
history ("#494, #495 and #496 all closed (#494/#495 via #730 ...)"). This
gate splits each block into clauses on `.`/`;` and skips a citation whose
own clause contains a historical marker word (`via`, `closed`, `closing`,
`landed`, `land[s]`, `fixed`, `resolved`, `merged`, `shipped`, `done`,
`already`). This is a word-list heuristic, not comprehension -- it is
deliberately biased to UNDER-flag: "if you cannot distinguish blocked-on
from landed-via, do not flag it" (#1950). A blocker phrased with one of
those words (e.g. "not yet fixed") can slip through unflagged; that is the
accepted cost of not spamming every "landed via #N" reference as a fresh
failure.

STALENESS -- this gate is OFFLINE
----------------------------------
No `gh`/network call happens here; a live call inside every PR run would
make this a network-flake gate on every PR touching metadata/**. Instead it
reads a COMMITTED snapshot (`metadata/issue-state-snapshot.json`) refreshed
on a schedule by `.github/workflows/refresh-issue-state-snapshot.yml` via
`scripts/refresh_issue_state_snapshot.py`. If that snapshot is missing,
unparsable, or older than `_STALE_AFTER_DAYS` days (the cron did not run),
every citation that would otherwise be an ERROR is reported as a WARNING
instead and the gate exits 0. A gate that hard-fails every PR because a cron
job didn't fire is worse than the drift it exists to catch (#1950).
A citation whose number has no entry in the snapshot at all (e.g. cited in
the same PR that adds it) is always a WARNING, never an ERROR -- the
snapshot cannot know about it yet.

Exit codes:
    0  no ERROR-level findings (WARNINGs may still be printed)
    1  at least one closed/merged issue cited as an open blocker, checked
       against a fresh snapshot
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from collections import namedtuple
from datetime import datetime, timedelta, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SNAPSHOT_RELPATH = Path("metadata") / "issue-state-snapshot.json"

#: A snapshot older than this is treated as unusable -- twice the weekly
#: refresh cadence, so one missed cron run doesn't immediately start
#: downgrading every finding.
_STALE_AFTER_DAYS = 14

#: `<anything>driver_status: value   # comment` -- matches the top-level
#: `driver_status:` field as well as nested siblings like
#: `nor_flash_driver_status:` / `emmc_driver_status:` (#1216's family).
_YAML_STATUS_FIELD_RE = re.compile(
    r"^\s*[A-Za-z0-9_]*driver_status\s*:\s*\S*\s*(?:#(.*))?$"
)
#: A line that is nothing but a trailing-comment continuation -- the shape
#: this repo's hand-wrapped YAML comments use (see gd32g553.yaml).
_YAML_CONT_RE = re.compile(r"^\s*#(.*)$")

#: `#NNNN`, not part of a longer token (so `PWM_CAPTURE` etc. never match).
_CITATION_RE = re.compile(r"(?<!\w)#(\d{1,6})\b")

#: Word-list historical marker -- see the HISTORICAL section above.
_HISTORICAL_RE = re.compile(
    r"\b(via|closed|closing|land(?:ed|s)?|fixed|resolved|merged|shipped|done|already)\b",
    re.IGNORECASE,
)


#: file: relpath (posix); line: 1-based; issue: int; historical: bool (see
#: HISTORICAL above); clause: the `.`/`;`-delimited clause the citation sits
#: in. A plain namedtuple, not @dataclass -- this module is loaded by tests
#: via `importlib.util.module_from_spec()` without registering itself in
#: `sys.modules`, and `@dataclass` on a module with
#: `from __future__ import annotations` resolves field types through
#: `sys.modules[cls.__module__]`, which is None there and raises.
Citation = namedtuple("Citation", "file line issue historical clause")


def _clause_around(text: str, start: int, end: int) -> str:
    """The `.`/`;`-delimited clause containing text[start:end]."""
    left = max(text.rfind(".", 0, start), text.rfind(";", 0, start))
    right_dot = text.find(".", end)
    right_semi = text.find(";", end)
    candidates = [x for x in (right_dot, right_semi) if x != -1]
    right = min(candidates) if candidates else len(text)
    return text[left + 1 : right + 1].strip()


def _citations_in_block(block_text: str) -> list[tuple[int, bool, str]]:
    out = []
    for m in _CITATION_RE.finditer(block_text):
        clause = _clause_around(block_text, m.start(), m.end())
        historical = bool(_HISTORICAL_RE.search(clause))
        out.append((int(m.group(1)), historical, clause))
    return out


def _iter_yaml_status_blocks(root: Path):
    """Yield (relpath, 1-based line, block text) for every
    `*driver_status:` field's trailing comment block under metadata/**."""
    meta_dir = root / "metadata"
    if not meta_dir.is_dir():
        return
    for path in sorted(meta_dir.rglob("*.yaml")):
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        i = 0
        while i < len(lines):
            m = _YAML_STATUS_FIELD_RE.match(lines[i])
            if not m:
                i += 1
                continue
            parts = [m.group(1) or ""]
            j = i + 1
            while j < len(lines):
                cm = _YAML_CONT_RE.match(lines[j])
                if not cm:
                    break
                parts.append(cm.group(1))
                j += 1
            block_text = " ".join(p.strip() for p in parts).strip()
            if block_text:
                yield path.relative_to(root), i + 1, block_text
            i = j


def _iter_header_status_blocks(root: Path):
    """Yield (relpath, 1-based line, block text) for every
    `@par Driver status:` Doxygen paragraph under include/alp/chips/."""
    headers_dir = root / "include" / "alp" / "chips"
    if not headers_dir.is_dir():
        return
    tag = "@par Driver status:"
    for path in sorted(headers_dir.glob("*.h")):
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        for i, line in enumerate(lines):
            idx = line.find(tag)
            if idx == -1:
                continue
            parts = [line[idx + len(tag) :]]
            j = i + 1
            while j < len(lines):
                stripped = lines[j].strip()
                if stripped.startswith("*/"):
                    break
                content = re.sub(r"^\*\s?", "", stripped)
                if content.startswith("@par "):
                    break
                parts.append(content)
                j += 1
            block_text = " ".join(p.strip() for p in parts).strip()
            if block_text:
                yield path.relative_to(root), i + 1, block_text


def find_citations(root: Path) -> list[Citation]:
    """Every `#NNNN` citation inside a driver-status prose block, in scope
    order (metadata/**/*.yaml, then include/alp/chips/*.h)."""
    out: list[Citation] = []
    for relpath, lineno, block in _iter_yaml_status_blocks(root):
        for num, hist, clause in _citations_in_block(block):
            out.append(Citation(relpath.as_posix(), lineno, num, hist, clause))
    for relpath, lineno, block in _iter_header_status_blocks(root):
        for num, hist, clause in _citations_in_block(block):
            out.append(Citation(relpath.as_posix(), lineno, num, hist, clause))
    return out


def _load_snapshot(path: Path) -> tuple[dict | None, str | None, list[str]]:
    if not path.is_file():
        return None, None, [
            f"no issue-state snapshot at {path} -- run "
            f"`python3 scripts/refresh_issue_state_snapshot.py` to create one"
        ]
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as e:
        return None, None, [f"{path}: not readable/valid JSON ({e})"]
    issues = doc.get("issues")
    generated_at = doc.get("generated_at")
    if not isinstance(issues, dict) or not isinstance(generated_at, str):
        return None, None, [
            f"{path}: missing 'issues' object or 'generated_at' string"
        ]
    return issues, generated_at, []


def _is_stale(generated_at: str) -> bool:
    try:
        ts = datetime.fromisoformat(generated_at.replace("Z", "+00:00"))
    except ValueError:
        return True
    if ts.tzinfo is None:
        ts = ts.replace(tzinfo=timezone.utc)
    return datetime.now(timezone.utc) - ts > timedelta(days=_STALE_AFTER_DAYS)


def _evaluate(root: Path, snapshot_path: Path) -> tuple[list[str], list[str]]:
    citations = find_citations(root)
    issues_map, generated_at, warnings = _load_snapshot(snapshot_path)
    warnings = list(warnings)

    stale = False
    if generated_at is not None:
        stale = _is_stale(generated_at)
        if stale:
            warnings.append(
                f"{snapshot_path}: snapshot is stale (generated_at="
                f"{generated_at!r}, older than {_STALE_AFTER_DAYS} days) -- "
                f"findings below are informational only until it refreshes"
            )

    usable = issues_map is not None and not stale
    errors: list[str] = []

    for c in citations:
        if c.historical:
            continue
        state = (issues_map or {}).get(str(c.issue))
        if state is None:
            warnings.append(
                f"{c.file}:{c.line}: cites #{c.issue} in a driver-status "
                f"block, but the snapshot has no record of it (new "
                f"citation, or the snapshot predates it) -- not enforced"
            )
            continue
        if state.strip().upper() != "OPEN":
            msg = (
                f"{c.file}:{c.line}: cites #{c.issue} ({state}) as an "
                f'apparent open blocker: "{c.clause}"'
            )
            if usable:
                errors.append(msg)
            else:
                warnings.append(msg + " [not enforced -- snapshot stale/missing]")

    return errors, warnings


def find_problems(root: Path, snapshot_path: Path | None = None) -> list[str]:
    """Blocking findings only. Empty on a clean tree, on a tree with no
    driver-status citations to a non-open issue, or whenever the snapshot
    is missing/stale (see STALENESS above) -- use `find_warnings` for the
    non-blocking picture."""
    if snapshot_path is None:
        snapshot_path = root / SNAPSHOT_RELPATH
    errors, _ = _evaluate(root, snapshot_path)
    return errors


def find_warnings(root: Path, snapshot_path: Path | None = None) -> list[str]:
    if snapshot_path is None:
        snapshot_path = root / SNAPSHOT_RELPATH
    _, warnings = _evaluate(root, snapshot_path)
    return warnings


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", type=Path, default=REPO)
    ap.add_argument("--snapshot", type=Path, default=None)
    args = ap.parse_args()

    snapshot_path = args.snapshot or (args.root / SNAPSHOT_RELPATH)
    errors, warnings = _evaluate(args.root, snapshot_path)

    for w in warnings:
        print(f"  WARN {w}")

    if errors:
        print(
            f"\ncheck-issue-citations: {len(errors)} closed/merged issue(s) "
            f"cited as open blocker(s) in driver-status prose:",
            file=sys.stderr,
        )
        for e in errors:
            print(f"  {e}", file=sys.stderr)
        print(
            "\nUpdate the prose to say what actually remains (see #1949 for "
            "the worked example), or drop the stale citation.",
            file=sys.stderr,
        )
        return 1

    print(
        f"check-issue-citations: OK -- {len(warnings)} warning(s), no "
        f"blocking findings."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
