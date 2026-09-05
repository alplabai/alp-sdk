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
    filed as a follow-up, #1958, if it turns out to be cheap.
  * a citation whose surrounding clause reads as historical rather than a
    blocker claim (see HISTORICAL below) -- reported as neither pass nor
    fail; it is simply not examined.
  * a citation whose number the snapshot has no record of at all -- degrades
    to a WARNING, never an ERROR (see STALENESS). Staleness alone does NOT
    do this: a stale-but-present snapshot still enforces every citation it
    can resolve.

HISTORICAL -- distinguishing "blocked on #N" from "landed via #N"
-------------------------------------------------------------------
The whole point of scoping to a driver-status block is that the block is BY
CONSTRUCTION a claim about what remains -- but the block can still narrate
history ("#494, #495 and #496 all closed (#494/#495 via #730 ...)"). This
gate splits each block into clauses on `.`/`;` and skips a citation whose
own clause contains a historical marker word (`via`, `closed`, `closing`,
`landed`, `land[s]`, `fixed`, `resolved`, `merged`, `shipped`, `done`,
`already`). Clause-splitting itself skips two false-boundary shapes before
looking for `.`/`;` (round 5, see `_mask_split_chars`): a `.` inside a
decimal or dotted version (`0.75 V`, `29.5 MHz`, `v0.3.x`) sitting between
the citation and its marker word used to end the clause before the marker
was ever reached (e.g. "#494 (the 0.75 V DEEPX core rail) was closed via
#730." split to "#494 (the 0."); so did a `;` used as an in-parenthetical
list separator rather than a real clause end (e.g. "#494 (full register
map; closed via #730)" split to "#494 (full register map;"). Both
misclassified a plainly-historical citation as a live blocker -- the
false-positive direction this gate must never take. This is a word-list
heuristic, not comprehension -- it is deliberately biased to UNDER-flag:
"if you cannot distinguish blocked-on from landed-via, do not flag it"
(#1958). A blocker phrased with one of those words (e.g. "not yet fixed")
can still slip through unflagged; that is the accepted cost of not
spamming every "landed via #N" reference as a fresh failure.

STALENESS -- this gate is OFFLINE
----------------------------------
No `gh`/network call happens here; a live call inside every PR run would
make this a network-flake gate on every PR touching metadata/**. Instead it
reads a COMMITTED snapshot (`metadata/issue-state-snapshot.json`), refreshed
by a human running `python3 scripts/refresh_issue_state_snapshot.py` --
there is no scheduled workflow, so ageing out is the expected steady state,
not a rare cron miss.  Because of that, staleness is DELIBERATELY NOT a
free pass: a citation whose number the snapshot DOES have on record is
enforced (ERROR) regardless of how old that record is -- an issue that was
CLOSED a year ago is still closed. Only two things ever downgrade a finding
to a non-blocking WARNING: (1) the snapshot is entirely missing/unparsable,
so nothing can be resolved at all, or (2) the cited number has no entry in
the snapshot yet (e.g. cited in the same PR that adds it) -- the snapshot
cannot know about a number it has never seen. A stale-but-present snapshot
additionally prints one WARNING per run naming the exact refresh command,
so the drift is visible without silently disabling enforcement.

TODAY'S ENFORCEMENT SURFACE -- read the "OK" line, not this paragraph
----------------------------------------------------------------------
On the tree this gate ships against, every existing driver-status citation
classifies as historical (see HISTORICAL above) -- #1949 already reworded
the one block that was phrased as a live blocker. So right now this gate's
live enforcement surface is zero: `harvested N, N historical, 0 evaluated`.
That is expected, not a bug -- it activates the next time someone writes a
blocker-phrased clause citing a since-closed issue, which is exactly the
defect class this gate exists to catch before it repeats.

Exit codes:
    0  no ERROR-level findings (WARNINGs may still be printed)
    1  at least one closed/merged issue cited as an open blocker whose
       number the snapshot has on record
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

#: A snapshot older than this earns a loud staleness WARNING (naming the
#: refresh command) -- it does NOT stop enforcement; refresh is a manual
#: `scripts/refresh_issue_state_snapshot.py` run now, so ageing out is the
#: expected steady state, not a rare miss (see STALENESS above).
_STALE_AFTER_DAYS = 14

#: `<anything>driver_status: value   # comment` -- matches the top-level
#: `driver_status:` field as well as nested siblings like
#: `nor_flash_driver_status:` / `emmc_driver_status:` (#1216's family).
_YAML_STATUS_FIELD_RE = re.compile(
    r"^\s*[A-Za-z0-9_]*driver_status\s*:\s*\S*\s*(?:#(.*))?$"
)
#: A line that is nothing but a trailing-comment continuation -- the shape
#: this repo's hand-wrapped YAML comments use (see gd32g553.yaml). Captures
#: the leading whitespace separately so the caller can require it to be
#: indented at least as far as the `driver_status:` field's own comment --
#: otherwise a comment-only banner belonging to the NEXT key (indented less,
#: e.g. back at column 0) gets swallowed into this block.
_YAML_CONT_RE = re.compile(r"^(\s*)#(.*)$")

#: `#NNNN`, not part of a longer token (so `PWM_CAPTURE` etc. never match).
_CITATION_RE = re.compile(r"(?<!\w)#(\d{1,6})\b")

#: Word-list historical marker -- see the HISTORICAL section above.
_HISTORICAL_RE = re.compile(
    r"\b(via|closed|closing|land(?:ed|s)?|fixed|resolved|merged|shipped|done|already)\b",
    re.IGNORECASE,
)

#: A decimal or dotted-version token -- `0.75`, `29.5`, `1.5`, `v0.3.x`.
#: `.`s inside one of these are never a clause boundary (round 5): naively
#: splitting clauses on ANY `.` treated the decimal point in a voltage/
#: frequency/version figure as a sentence end, truncating the clause before
#: it ever reached a historical marker word sitting later in the same
#: sentence (e.g. "#494 (the 0.75 V DEEPX core rail) was closed via #730."
#: split to "#494 (the 0.", never reaching "closed via").
_DOTTED_NUM_RE = re.compile(r"\b[vV]?\d+(?:\.(?:\d+|[a-zA-Z]))+\b")


def _mask_split_chars(text: str) -> str:
    """Return `text` with the `.`s inside a `_DOTTED_NUM_RE` token, and any
    `.`/`;` sitting inside an unmatched `(...)` span, replaced by a NUL
    placeholder -- so `_clause_around`'s boundary search skips them without
    disturbing `text`'s length/positions (round 5, see `_DOTTED_NUM_RE`).
    A parenthetical aside -- however many real sentences of old prose it
    quotes, e.g. "#494 (<the entire old multi-sentence driver_status
    comment>) was closed via #730." -- reads as ONE clause belonging to the
    citation, not one ending at its own first internal full stop; splitting
    on an in-parenthetical `.`/`;` reproduces the same false-positive shape
    as a decimal point, just with real sentence punctuation instead of a
    number, so it gets the same treatment.

    KNOWN LIMIT, deliberately not "fixed" -- an UNBALANCED `(` leaves the
    depth counter above zero for the rest of the block, so every later
    `.`/`;` is masked and the whole block collapses into one clause.  A
    genuine blocker sitting after a historical marker can then read as
    historical and go UNFLAGGED.  Measured on the masking itself:

        "a (b. c; d. e"   ->  "a (b@ c@ d@ e"    3 boundaries masked
        "a (b. c; d.) e"  ->  "a (b@ c@ d@) e"   3 boundaries masked
        "a b. c; d. e"    ->  unchanged           0 masked

    i.e. the unmatched `(` masks exactly as much as a closed one, for the
    whole remainder.  Tracking only balanced spans would close that, but it
    would swap a silent miss for a
    hard CI failure on prose with a stray `(` -- and this gate is `gate:
    true`, so a false positive blocks the merge queue while a false negative
    only fails to catch a stale citation.  Under-flagging is this module's
    documented bias, so the miss is the correct trade.  Zero occurrences on
    the tree today (all 77 status blocks checked); written down rather than
    silently relied upon, because a gate whose limits are not stated gets
    trusted for things it cannot do."""
    chars = list(text)
    for m in _DOTTED_NUM_RE.finditer(text):
        for i in range(m.start(), m.end()):
            if chars[i] == ".":
                chars[i] = "\x00"
    depth = 0
    for i, ch in enumerate(text):
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth = max(0, depth - 1)
        elif ch in ".;" and depth > 0:
            chars[i] = "\x00"
    return "".join(chars)


#: file: relpath (posix); line: 1-based; issue: int; historical: bool (see
#: HISTORICAL above); clause: the `.`/`;`-delimited clause the citation sits
#: in. A plain namedtuple, not @dataclass -- this module is loaded by tests
#: via `importlib.util.module_from_spec()` without registering itself in
#: `sys.modules`, and `@dataclass` on a module with
#: `from __future__ import annotations` resolves field types through
#: `sys.modules[cls.__module__]`, which is None there and raises.
Citation = namedtuple("Citation", "file line issue historical clause")


def _clause_around(text: str, start: int, end: int) -> str:
    """The `.`/`;`-delimited clause containing text[start:end]. Boundary
    search runs over a masked copy (see `_mask_split_chars`) so a decimal,
    dotted version, or in-parenthetical `;` near the citation is never
    mistaken for a sentence end (round 5); the returned slice is still cut
    from the original, unmasked `text`."""
    masked = _mask_split_chars(text)
    left = max(masked.rfind(".", 0, start), masked.rfind(";", 0, start))
    right_dot = masked.find(".", end)
    right_semi = masked.find(";", end)
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


def _iter_yaml_status_blocks(root: Path, warnings: list[str] | None = None):
    """Yield (relpath, 1-based line, block text) for every
    `*driver_status:` field's trailing comment block under metadata/**.

    `warnings`, if given, collects one entry per continuation line that
    LOOKS like an attempted continuation (indented further than the
    `driver_status:` line itself) but falls short of the field's own `#`
    column -- a one-space hand-wrapping slip that would otherwise end the
    block silently, with zero diagnostic (#1950 round 3). A continuation
    indented no further than the field line's own leading whitespace is a
    different key's banner comment, not a slipped continuation, and stays
    silent -- see `test_next_key_banner_not_swallowed_*`.

    The field line itself having NO trailing `#` at all (`hash_col == -1`,
    true for most `driver_status:`/`nor_flash_driver_status:`/
    `emmc_driver_status:` fields on the real tree) is the same slip, not an
    exemption: an indented comment line right after such a field is just as
    unreachable-without-a-warning as a one-column-short one would be, so it
    is treated as falling short of every column rather than skipped
    entirely (#1950 round 4) -- see
    `test_indentation_drift_warns_when_field_has_no_trailing_comment`."""
    meta_dir = root / "metadata"
    if not meta_dir.is_dir():
        return
    for path in sorted(meta_dir.rglob("*.yaml")):
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError:
            continue
        relpath = path.relative_to(root)
        i = 0
        while i < len(lines):
            m = _YAML_STATUS_FIELD_RE.match(lines[i])
            if not m:
                i += 1
                continue
            parts = [m.group(1) or ""]
            hash_col = lines[i].find("#")
            field_indent = len(lines[i]) - len(lines[i].lstrip())
            j = i + 1
            while j < len(lines):
                cm = _YAML_CONT_RE.match(lines[j])
                if not cm:
                    break
                indent = len(cm.group(1))
                if hash_col == -1 or indent < hash_col:
                    if warnings is not None and indent > field_indent:
                        if hash_col == -1:
                            warnings.append(
                                f"{relpath.as_posix()}:{j + 1}: comment "
                                f"continuation follows a driver_status field "
                                f"with no trailing comment of its own to "
                                f"continue -- treated as ending the block; "
                                f"this line and everything after it were NOT "
                                f"read as part of it. Add a `#` comment on "
                                f"the driver_status line itself if this "
                                f"belongs there."
                            )
                        else:
                            warnings.append(
                                f"{relpath.as_posix()}:{j + 1}: comment "
                                f"continuation is indented to column {indent}, "
                                f"short of the driver_status field's own "
                                f"comment at column {hash_col} -- treated as "
                                f"ending the block; this line and everything "
                                f"after it were NOT read as part of it. Align "
                                f"it to column {hash_col} if it belongs there."
                            )
                    break
                parts.append(cm.group(2))
                j += 1
            block_text = " ".join(p.strip() for p in parts).strip()
            if block_text:
                yield relpath, i + 1, block_text
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


def find_citations(root: Path, warnings: list[str] | None = None) -> list[Citation]:
    """Every `#NNNN` citation inside a driver-status prose block, in scope
    order (metadata/**/*.yaml, then include/alp/chips/*.h). `warnings`, if
    given, collects non-blocking diagnostics from the harvest itself (e.g.
    an indentation-drift-truncated block, see `_iter_yaml_status_blocks`)
    -- separate from the state-lookup warnings `_evaluate` adds."""
    out: list[Citation] = []
    for relpath, lineno, block in _iter_yaml_status_blocks(root, warnings):
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


def _evaluate(
    root: Path, snapshot_path: Path
) -> tuple[list[str], list[str], dict[str, int]]:
    """Enforce every citation the snapshot CAN resolve, regardless of its
    age -- a closed issue stays closed. Staleness only earns one loud,
    non-blocking WARNING naming the refresh command; it never silently
    downgrades an otherwise-resolvable ERROR (see STALENESS in the module
    docstring, #1950 round 2).

    The third return value is a harvest/evaluation count breakdown (#1950
    round 3, so `main()`'s "OK" output can say what it actually did): how
    many citations were harvested at all, how many were historical (skipped,
    never examined), and how many were actually evaluated against the
    snapshot (the rest have no record in the snapshot yet -- a warning, not
    an evaluation)."""
    warnings: list[str] = []
    citations = find_citations(root, warnings)
    issues_map, generated_at, snapshot_warnings = _load_snapshot(snapshot_path)
    warnings.extend(snapshot_warnings)

    if generated_at is not None and _is_stale(generated_at):
        warnings.append(
            f"{snapshot_path}: snapshot is stale (generated_at="
            f"{generated_at!r}, older than {_STALE_AFTER_DAYS} days) -- "
            f"still enforced against every citation it has a record for; "
            f"run `python3 scripts/refresh_issue_state_snapshot.py` to "
            f"bring it current"
        )

    errors: list[str] = []
    historical = 0
    evaluated = 0

    for c in citations:
        if c.historical:
            historical += 1
            continue
        state = (issues_map or {}).get(str(c.issue))
        if state is None:
            warnings.append(
                f"{c.file}:{c.line}: cites #{c.issue} in a driver-status "
                f"block, but the snapshot has no record of it (new "
                f"citation, or the snapshot predates it) -- not enforced"
            )
            continue
        evaluated += 1
        if state.strip().upper() != "OPEN":
            errors.append(
                f"{c.file}:{c.line}: cites #{c.issue} ({state}) as an "
                f'apparent open blocker: "{c.clause}"'
            )

    stats = {"harvested": len(citations), "historical": historical, "evaluated": evaluated}
    return errors, warnings, stats


def find_problems(root: Path, snapshot_path: Path | None = None) -> list[str]:
    """Blocking findings only. Empty on a clean tree, on a tree with no
    driver-status citations to a non-open issue, or whenever the snapshot
    is entirely missing/unparsable or has no record of the cited number
    yet (staleness alone does NOT empty this -- see STALENESS above) --
    use `find_warnings` for the non-blocking picture."""
    if snapshot_path is None:
        snapshot_path = root / SNAPSHOT_RELPATH
    errors, _, _ = _evaluate(root, snapshot_path)
    return errors


def find_warnings(root: Path, snapshot_path: Path | None = None) -> list[str]:
    if snapshot_path is None:
        snapshot_path = root / SNAPSHOT_RELPATH
    _, warnings, _ = _evaluate(root, snapshot_path)
    return warnings


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", type=Path, default=REPO)
    ap.add_argument("--snapshot", type=Path, default=None)
    args = ap.parse_args()

    snapshot_path = args.snapshot or (args.root / SNAPSHOT_RELPATH)
    errors, warnings, stats = _evaluate(args.root, snapshot_path)

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
        f"check-issue-citations: OK -- harvested {stats['harvested']} "
        f"citation(s), {stats['historical']} historical (not evaluated), "
        f"{stats['evaluated']} evaluated against the snapshot, "
        f"{len(warnings)} warning(s), no blocking findings."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
