#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""Guard: a changelog.d/ fragment's leading digits match the issue its own
heading cites (#1957).

WHY THIS EXISTS
---------------
`changelog.d/README.md` documents the leading digits of a fragment's
filename as the join key back to its GitHub issue -- but until now nothing
checked that those digits actually agree with the `(#N)` the fragment's own
`### ... (#N)` heading cites. Four fragments on `dev` disagreed:
`changelog.d/1909.md` cited `(#1700)`, `changelog.d/1917.md` cited
`(#1648)`, `changelog.d/1932.md` cited `(#1909)`, and `changelog.d/1940.md`
cited `(#1848, #1814)` -- the last of those was invisible to the original
single-citation-only check because it cites two issue numbers, and the
guard tightened below is what makes it checkable at all.

That mismatch was not cosmetic. `changelog.d/1909.md` holding a `#1700`
entry is what *caused* the collision alp-sdk#1941 exists to fix: the 1909
slot was occupied by a fragment belonging to a different issue, so the real
#1909 fix had nowhere to go and was filed under the PR number instead
(`changelog.d/1932.md`). A join-key invariant that nothing enforces is not
an invariant.

WHAT IT CATCHES, AND WHAT IT DOES NOT -- read this before trusting it
--------------------------------------------------------------------
CATCHES:
  * a fragment whose first non-blank line (its `### ...` heading) cites
    EXACTLY ONE distinct `#N`, and that N does not match the fragment
    filename's leading digits.
  * a fragment whose heading cites MORE THAN ONE distinct `#N`, where the
    filename's leading digits are neither one of the cited numbers NOR
    inside the [min, max] span between them -- `changelog.d/1940.md`
    citing `(#1848, #1814)` is this case: 1940 is outside both `{1848,
    1814}` and the range `1814..1848`, so it cannot be either issue's
    fragment and is flagged even though the heading names two issues.

DOES NOT CATCH, on purpose -- under-flag on ambiguity:
  * a heading with NO `#N` citation at all -- `changelog.d/813.md` and
    `changelog.d/853.md`/`changelog.d/1652.md` (the latter two are bare
    `### Added`/`### Changed` bullet-body fragments with no title, a shape
    `changelog.d/README.md` does not document) all fall here, as does
    `changelog.d/1949.md` (a titled heading that simply never cites an
    issue number). There is nothing to compare the filename against, so
    nothing is flagged. The same is true of a heading citing an issue via
    the `alp-sdk#N` form (e.g. `(alp-sdk#1957)`): `_HEADING_ISSUE_RE`
    requires a non-word character right before `#`, and the `k` immediately
    before it there fails that, so the citation is not recognized either
    and the fragment is treated as citing nothing.
  * a heading citing MORE THAN ONE DISTINCT issue number, where the
    filename's leading digits fall inside the [min, max] span of the cited
    numbers -- e.g. a range (`changelog.d/1761.md`'s heading reads
    `(#1757-#1783)`, and 1757 <= 1761 <= 1783) or a list of several issues
    closed by one sweep. Which one, if any, is "the" issue the filename
    should match is not decidable from the heading alone in that case, so
    it is left unflagged rather than guessed at. A heading repeating the
    SAME number more than once (`changelog.d/1818.md` cites `#1818` twice)
    is not ambiguous by this rule -- it collapses to one distinct value and
    is checked as the single-citation case above.
  * anything in the fragment's BODY. A fragment legitimately cites many
    issues in its prose (a root-cause writeup, a "see also"); only the
    heading is a claim about which issue this fragment's file slot belongs
    to, so only the heading is scoped.
  * a fragment whose filename does not even start with digits, or whose
    first line is not a `### ` heading at all -- `scripts/check_changelog_
    fragments.py` already owns flagging those structural defects; this gate
    only compares two numbers once both exist.

This is a word/number-count heuristic, not comprehension -- it is
deliberately biased to UNDER-flag, matching this repo's other citation
gates (`scripts/check_issue_citations.py`, `scripts/check_changelog_
citations.py`): a mismatch this gate cannot confidently resolve is not
reported, rather than reported wrong.

Exit codes:
    0  no fragment's heading citation contradicts its filename
    1  at least one fragment's heading cites a different issue than its
       filename's leading digits
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

#: `#NNNN`, not part of a longer token (so `PWM_CAPTURE` etc. never match,
#: and so does an `alp-sdk#N`-form citation -- the `k` before `#` is a word
#: char too) -- similar shape to `check_issue_citations.py`'s
#: `_CITATION_RE` (`(?<!\w)#(\d{1,6})\b`), but this one has no digit-count
#: cap (`\d+`, not `\d{1,6}`).
_HEADING_ISSUE_RE = re.compile(r"(?<!\w)#(\d+)\b")

#: The leading digits of a fragment's filename stem -- the join key
#: (`changelog.d/README.md`), same extraction `assemble_changelog.py`'s
#: `_fragment_sort_key` uses.
_LEADING_DIGITS_RE = re.compile(r"^(\d+)")


def _heading_line(text: str) -> str | None:
    """The fragment's first non-blank line -- its `### ...` heading, or
    None for an empty/whitespace-only file (already flagged elsewhere by
    `check_changelog_fragments.py`)."""
    for line in text.splitlines():
        if line.strip():
            return line
    return None


def _iter_fragments(frag_dir: Path) -> list[Path]:
    if not frag_dir.is_dir():
        return []
    return sorted(p for p in frag_dir.glob("*.md") if p.name != "README.md")


def find_problems(root: Path) -> list[str]:
    frag_dir = root / "changelog.d"
    problems: list[str] = []

    for path in _iter_fragments(frag_dir):
        heading = _heading_line(
            path.read_text(encoding="utf-8", errors="replace")
        )
        if heading is None or not heading.lstrip().startswith("###"):
            continue  # structural defect; not this gate's job to flag

        leading = _LEADING_DIGITS_RE.match(path.stem)
        if leading is None:
            continue  # malformed filename; not this gate's job to flag

        cited = sorted(set(int(n) for n in _HEADING_ISSUE_RE.findall(heading)))
        if not cited:
            continue  # no citation to compare against -- under-flag

        filename_n = int(leading.group(1))

        if len(cited) == 1:
            if cited[0] != filename_n:
                problems.append(
                    f"{path.name}: filename leads with issue #{leading.group(1)}, "
                    f"but its own heading cites (#{cited[0]}) -- rename the "
                    f"fragment to `{cited[0]}.md` (or `{cited[0]}-<slug>.md` if "
                    f"that issue already has a fragment) so the leading digits "
                    f"stay the join key back to the issue (changelog.d/README.md)"
                )
            continue

        # More than one distinct citation: which one, if any, is "the" issue
        # is not decidable from the heading alone (a range like
        # `#1757-#1783`, or a list of several issues one sweep closed) --
        # UNLESS the filename number is not even a candidate: not one of the
        # cited numbers, and not inside the [min, max] span between them
        # either (a genuine range citation legitimately covers every number
        # in between, so falling inside the span is not a mismatch). Outside
        # both is not ambiguity, it is simply the wrong slot.
        if filename_n not in cited and not (cited[0] <= filename_n <= cited[-1]):
            cited_list = ", ".join(f"#{n}" for n in cited)
            problems.append(
                f"{path.name}: filename leads with issue #{filename_n}, but "
                f"its own heading cites {cited_list} -- that is neither one "
                f"of those issues nor within the range between them, so "
                f"#{filename_n} cannot be the issue this fragment belongs "
                f"to; rename it to the issue it actually documents "
                f"(changelog.d/README.md)"
            )

    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--root", type=Path, default=REPO,
                     help="repo root to scan (default: the real repo)")
    args = ap.parse_args()

    problems = find_problems(args.root)
    if problems:
        for p in problems:
            print(f"changelog-fragment-issue: {p}", file=sys.stderr)
        return 1
    print("OK: every changelog.d/ fragment's heading citation agrees with "
          "its filename (where checkable).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
