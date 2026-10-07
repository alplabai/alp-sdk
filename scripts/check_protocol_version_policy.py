#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Protocol version policy gate for the CC3501E wire (ADR 0033).

Bumping the version number is the path of least resistance whenever someone
touching the CC3501E wire protocol is unsure whether a change is safe: a
MAJOR bump can never be technically WRONG, so nothing costs it anything, and
every forced reflash it causes is invisible to whoever ships it. That is
exactly how the protocol went v5 -> v9 in a single week (2026-08-27 ->
2026-09-03) with FOUR forced host+firmware reflashes, when ADR 0033's own
retroactive analysis shows only TWO of those four bumps (v7, v8) could
actually misread an old host -- v6 and v9 were purely additive and cost a
reflash for nothing.

ADR 0033 fixes the technical half (MAJOR gates the link, MINOR does not).
This gate fixes the social half: a MAJOR bump must be justified in writing
before it lands, so the friction sits where the decision is made, not
downstream in a customer's forced reflash.

Checks `include/alp/protocol/cc3501e.h`'s ALP_CC3501E_PROTOCOL_MAJOR /
_MINOR against the machine-readable "## Version ledger" fenced block in
docs/adr/0033-cc3501e-wire-protocol-is-versioned-major-minor-with-capabilities.md
-- the ADR's prose version-history table (under Context) is for humans and is
deliberately NOT this gate's parse target. FAILS when:

  - the ledger section or its fenced block is missing, or a row is
    malformed (want `MAJOR.MINOR = MAJOR|MINOR = justification`);
  - the header's MAJOR.MINOR has no matching ledger row;
  - the ledger's newest row disagrees with the header;
  - the ledger is not strictly increasing, row over row;
  - any row's MAJOR is 0 (reserved for pre-scheme firmware -- see the
    header's ALP_CC3501E_PROTOCOL_VERSION comment for why);
  - a row marked MAJOR carries no justification containing "misread" --
    ADR 0033's own test for a MAJOR bump ("an existing host, unchanged,
    would be misread by the new firmware, or would misread its replies").

Exit 0 with one OK line when the ledger and the header agree.
"""
from __future__ import annotations

import re
import sys
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

_HEADER_REL = "include/alp/protocol/cc3501e.h"
_ADR_REL = (
    "docs/adr/0033-cc3501e-wire-protocol-is-versioned-major-minor-with-capabilities.md"
)

_MAJOR_DEFINE_RE = re.compile(r"^#define\s+ALP_CC3501E_PROTOCOL_MAJOR\s+(\d+)", re.MULTILINE)
_MINOR_DEFINE_RE = re.compile(r"^#define\s+ALP_CC3501E_PROTOCOL_MINOR\s+(\d+)", re.MULTILINE)

_LEDGER_HEADING_RE = re.compile(r"^## Version ledger\s*$", re.MULTILINE)
_TOP_HEADING_RE = re.compile(r"^## ", re.MULTILINE)
_FENCE_RE = re.compile(r"```[^\n]*\n(.*?)```", re.DOTALL)

# `MAJOR.MINOR = MAJOR|MINOR = justification`, e.g.
# `3.1 = MINOR = added SOCK_BIND/SOCK_LISTEN/EVT_SOCK_ACCEPTED, ...`.
_ROW_RE = re.compile(
    r"^(?P<major>\d+)\.(?P<minor>\d+)\s*=\s*(?P<bump>MAJOR|MINOR)\s*=\s*(?P<just>.+)$"
)


@dataclass(frozen=True)
class LedgerRow:
    major: int
    minor: int
    bump: str
    justification: str
    raw: str

    @property
    def version(self) -> tuple[int, int]:
        return (self.major, self.minor)


def parse_header_version(header_text: str) -> tuple[int | None, int | None, list[str]]:
    """Resolve ALP_CC3501E_PROTOCOL_MAJOR/_MINOR; missing macros are
    reported problems, never a silent None that later checks skip over."""
    m_major = _MAJOR_DEFINE_RE.search(header_text)
    m_minor = _MINOR_DEFINE_RE.search(header_text)
    problems = []
    if m_major is None:
        problems.append(
            f"{_HEADER_REL}: no '#define ALP_CC3501E_PROTOCOL_MAJOR <n>' found"
        )
    if m_minor is None:
        problems.append(
            f"{_HEADER_REL}: no '#define ALP_CC3501E_PROTOCOL_MINOR <n>' found"
        )
    major = int(m_major.group(1)) if m_major else None
    minor = int(m_minor.group(1)) if m_minor else None
    return major, minor, problems


def extract_ledger_block(adr_text: str) -> tuple[str | None, str | None]:
    """Return (fenced-block text, problem) -- exactly one is None. Fails
    loudly rather than returning an empty block when the heading or the
    fence it must contain isn't there, per this repo's gate convention: a
    gate that returns clean because a regex missed its target is worse
    than no gate."""
    heading = _LEDGER_HEADING_RE.search(adr_text)
    if heading is None:
        return None, (
            f"{_ADR_REL}: no '## Version ledger' section -- see ADR 0033 "
            f"Decision point 4"
        )
    section_end_m = _TOP_HEADING_RE.search(adr_text, heading.end())
    section = adr_text[heading.end() : section_end_m.start() if section_end_m else len(adr_text)]
    fence = _FENCE_RE.search(section)
    if fence is None:
        return None, (
            f"{_ADR_REL}: '## Version ledger' section has no fenced code "
            f"block to parse"
        )
    return fence.group(1), None


def parse_ledger_rows(block: str) -> tuple[list[LedgerRow], list[str]]:
    """Parse every non-blank line of the ledger's fenced block. A line
    that doesn't match `_ROW_RE` is a reported problem, never a silently
    dropped row -- same reasoning as extract_ledger_block()."""
    rows: list[LedgerRow] = []
    problems: list[str] = []
    for lineno, line in enumerate(block.splitlines(), start=1):
        if not line.strip():
            continue
        m = _ROW_RE.match(line)
        if m is None:
            problems.append(
                f"{_ADR_REL}: Version ledger row {lineno} is malformed "
                f"(want 'MAJOR.MINOR = MAJOR|MINOR = justification'): "
                f"{line.strip()!r}"
            )
            continue
        rows.append(
            LedgerRow(
                major=int(m.group("major")),
                minor=int(m.group("minor")),
                bump=m.group("bump"),
                justification=m.group("just").strip(),
                raw=line.strip(),
            )
        )
    return rows, problems


def validate_rows(rows: list[LedgerRow]) -> list[str]:
    """MAJOR-0 reservation, the MAJOR/"misread" justification rule, and
    strictly-increasing order -- everything the ledger must hold on its
    own, before it's even compared against the header."""
    problems: list[str] = []
    prev: LedgerRow | None = None
    for row in rows:
        if row.major == 0:
            problems.append(
                f"{_ADR_REL}: Version ledger row {row.raw!r} -- MAJOR 0 is "
                f"reserved for pre-scheme firmware (see {_HEADER_REL}'s "
                f"ALP_CC3501E_PROTOCOL_VERSION comment) and must never "
                f"appear in a real ledger row"
            )
        if row.bump == "MAJOR" and "misread" not in row.justification.lower():
            problems.append(
                f"{_ADR_REL}: Version ledger row {row.raw!r} is marked "
                f"MAJOR but its justification does not say an old host "
                f"would be misread -- that is ADR 0033's own test for a "
                f"MAJOR bump ('an existing host, unchanged, would be "
                f"misread by the new firmware, or would misread its "
                f"replies'); either add that justification, or this "
                f"change was actually additive and should be MINOR"
            )
        if prev is not None and row.version <= prev.version:
            problems.append(
                f"{_ADR_REL}: Version ledger is not monotonically "
                f"increasing -- row {row.raw!r} does not come strictly "
                f"after {prev.raw!r}"
            )
        prev = row
    return problems


def find_problems(root: Path) -> list[str]:
    header_path = root / _HEADER_REL
    adr_path = root / _ADR_REL

    if not header_path.is_file():
        return [f"{_HEADER_REL}: file not found"]
    if not adr_path.is_file():
        return [f"{_ADR_REL}: file not found"]

    header_major, header_minor, header_problems = parse_header_version(
        header_path.read_text(encoding="utf-8")
    )
    if header_problems:
        return header_problems

    block, block_problem = extract_ledger_block(adr_path.read_text(encoding="utf-8"))
    if block_problem:
        return [block_problem]
    assert block is not None

    rows, row_problems = parse_ledger_rows(block)
    if row_problems:
        return row_problems
    if not rows:
        return [f"{_ADR_REL}: Version ledger fenced block has no rows"]

    structural_problems = validate_rows(rows)
    if structural_problems:
        return structural_problems

    header_version = (header_major, header_minor)
    if not any(row.version == header_version for row in rows):
        return [
            f"{_HEADER_REL} declares MAJOR.MINOR "
            f"{header_major}.{header_minor}, but {_ADR_REL}'s Version "
            f"ledger has no matching row -- add one before landing this "
            f"bump (and if it's a MAJOR bump, justify it with an "
            f"'old host would be misread' sentence)"
        ]

    newest = rows[-1]
    if newest.version != header_version:
        return [
            f"{_ADR_REL}'s newest Version ledger row is {newest.raw!r} "
            f"({newest.major}.{newest.minor}), but {_HEADER_REL} declares "
            f"{header_major}.{header_minor} -- the ledger's last row must "
            f"be the header's current version"
        ]

    return []


def main() -> int:
    problems = find_problems(ROOT)
    if problems:
        print(
            "check_protocol_version_policy: CC3501E wire protocol version "
            "policy violation (ADR 0033):",
            file=sys.stderr,
        )
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        print(
            f"\ncheck_protocol_version_policy: {len(problems)} problem(s) "
            f"-- failing.",
            file=sys.stderr,
        )
        return 1

    print(
        f"check_protocol_version_policy: OK ({_HEADER_REL}'s "
        f"ALP_CC3501E_PROTOCOL_MAJOR.MINOR matches {_ADR_REL}'s Version "
        f"ledger; ledger is monotonic and every MAJOR row is justified)."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
