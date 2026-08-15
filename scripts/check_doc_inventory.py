#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Doc-inventory gate -- fails (exit 1) when a hand-maintained inventory
count or workflow-table entry in the docs drifts from the tree it
describes (issue #1265, following up on #1209's fifth required-work item).

Four ANCHORED, single-fact claims are checked:

  (a) `docs/ci/README.md`'s "carries **N** workflow files" count.
  (b) `docs/ci/README.md`'s "## Workflows shipped" table -- each row links
      `` [`name.yml`](../../.github/workflows/name.yml) ``; the displayed
      name must match the linked file's own name.  (Whether the link
      *resolves* is scripts/check_doc_links.py's job -- it already scans
      docs/ci/** for dead relative paths in the same pr-doc-drift.yml job;
      duplicating that here would just fail the same rename twice.)
  (c) `docs/README.md`'s "(N ADRs; recount with ...)" docs-index count.
  (d) `docs/contributing-tier-2.md`'s "N chips, M libraries" Tier-1 count.

Scope is deliberately narrower than #1265's initial wording -- these four
claims (a bold count, a `(N ADRs;` parenthetical, an `N chips, M libraries`
line, a `[`file`](path)` link pair) are matched via regex/markdown-link
matching, not prose parsing.  Two invariants every check below holds, both
because a hand-kept inventory count that stops enforcing itself is worse
than no gate:

  * An anchor that can't be found is a hard FAILURE
    (`problems.append(...)`), never a silent skip.  See `find_problems()`
    below; there is no code path that returns `[]` because a doc was
    unreadable or a pattern didn't match.
  * Every occurrence of an anchor pattern in the doc is checked, not just
    the first.  A count-claim regex uses `finditer()` and compares EACH
    match against the tree; a doc that states the count twice (once
    correctly, once stale) fails on the stale occurrence regardless of
    which one comes first in the file.

Deliberately NOT covered (the same principle applied to the sibling gate
lives in #1264's own gate, scripts/check_agents_md_generators.py):

  * A blanket "every `*.yml`/`*.yaml` filename mentioned anywhere in
    docs/ci/HW-IN-LOOP.md, docs/testing.md, docs/test-plan.md must exist
    in .github/workflows/" sweep.  The current tree already falsifies
    that idea: `docs/ci/HW-IN-LOOP.md` legitimately names
    `nightly-aen-hil.yml` in a "(History: ... and the workflow was
    deleted)" parenthetical, and `docs/test-plan.md`'s CI-only-rows table
    links to `alplabai/alp-sdk-vscode`'s OWN `ci.yml`, a different repo's
    workflow entirely.  A context-blind existence sweep flags both as
    dead -- exactly the "flags legitimate prose" failure mode that trains
    people to bypass a gate.  The one table this gate DOES check
    (`docs/ci/README.md`'s "Workflows shipped") is safe to check
    structurally because only a real markdown LINK commits to "this
    workflow exists now"; the "Workflows planned" table two headings
    below it uses bare backticks with no link syntax for exactly this
    reason, and is correctly never matched by `_SHIPPED_LINK_RE`.
  * `docs/testing.md` / `docs/test-plan.md` workflow-count claims -- #1265
    named them as candidates, but neither file states a *count* today
    (only individual filenames in tables, covered by the point above).
    A future doc that starts hand-counting workflows there is new scope,
    not a gap in this gate.

Run locally:

    python3 scripts/check_doc_inventory.py

Exits non-zero if any of the four counts/names drifted from the tree.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

_WORKFLOW_COUNT_RE = re.compile(r"carries \*\*(\d+)\*\* workflow files")
_SHIPPED_LINK_RE = re.compile(r"\[`([A-Za-z0-9_.-]+\.ya?ml)`\]\(([^)]+)\)")
_ADR_COUNT_RE = re.compile(r"\((\d+) ADRs;")
_CHIP_LIBRARY_COUNT_RE = re.compile(r"(\d+) chips,\s*(\d+) libraries")


def _real_workflow_count(root: Path) -> int:
    """.github/workflows/*.yml AND *.yaml -- GitHub Actions reads both
    extensions even though every workflow in this tree today is .yml."""
    workflows_dir = root / ".github" / "workflows"
    return len(list(workflows_dir.glob("*.yml"))) + len(list(workflows_dir.glob("*.yaml")))


def _line_no(text: str, offset: int) -> int:
    return text.count("\n", 0, offset) + 1


def find_workflow_count_drift(root: Path) -> list[str]:
    doc = root / "docs" / "ci" / "README.md"
    if not doc.is_file():
        return ["docs/ci/README.md: not found -- cannot verify the workflow count"]
    text = doc.read_text(encoding="utf-8")
    matches = list(_WORKFLOW_COUNT_RE.finditer(text))
    if not matches:
        return ["docs/ci/README.md: could not find the 'carries **N** workflow "
                 "files' line -- update this gate's anchor if the wording changed"]
    workflows_dir = root / ".github" / "workflows"
    if not workflows_dir.is_dir():
        return [".github/workflows: not found -- cannot verify the workflow count"]
    real = _real_workflow_count(root)
    problems: list[str] = []
    # Every occurrence is checked, not just the first: re.search() picking
    # the first match let a correct decoy earlier in the file shadow a
    # stale claim later (or vice versa) -- see the module docstring.
    for m in matches:
        stated = int(m.group(1))
        if stated != real:
            problems.append(
                f"docs/ci/README.md:{_line_no(text, m.start())}: states "
                f"**{stated}** workflow files, .github/workflows/ has {real} "
                f"(*.yml + *.yaml)")
    return problems


def find_workflow_link_drift(root: Path) -> list[str]:
    """Each '## Workflows shipped' row's link TEXT must match its own
    href's filename.  Whether the href resolves is check_doc_links.py's
    job (it already scans docs/ci/** for dead relative paths in the same
    pr-doc-drift.yml job) -- not duplicated here."""
    doc = root / "docs" / "ci" / "README.md"
    if not doc.is_file():
        return []  # already reported by find_workflow_count_drift
    problems: list[str] = []
    text = doc.read_text(encoding="utf-8")
    for line_no, line in enumerate(text.splitlines(), 1):
        for m in _SHIPPED_LINK_RE.finditer(line):
            name, href = m.group(1), m.group(2)
            if href.startswith(("http://", "https://")):
                continue  # a different repo's workflow, not this tree's
            # Strip a #fragment / ?query before taking the basename -- a
            # link like `../../.github/workflows/pr-twister.yml#L20` is a
            # legitimate deep link, not a filename mismatch.
            href_path = href.split("#", 1)[0].split("?", 1)[0]
            href_name = Path(href_path).name
            if href_name != name:
                problems.append(
                    f"docs/ci/README.md:{line_no}: link text `{name}` doesn't "
                    f"match its href's filename ({href_name})")
    return problems


def find_adr_count_drift(root: Path) -> list[str]:
    doc = root / "docs" / "README.md"
    if not doc.is_file():
        return ["docs/README.md: not found -- cannot verify the ADR count"]
    text = doc.read_text(encoding="utf-8")
    matches = list(_ADR_COUNT_RE.finditer(text))
    if not matches:
        return ["docs/README.md: could not find the '(N ADRs; recount ...)' "
                 "line -- update this gate's anchor if the wording changed"]
    adr_dir = root / "docs" / "adr"
    if not adr_dir.is_dir():
        return ["docs/adr: not found -- cannot verify the ADR count"]
    # Mirrors the doc's own recount command, `ls docs/adr/[0-9]*.md | wc -l`
    # -- numbered ADR files only, not docs/adr/README.md.
    real = len([p for p in adr_dir.glob("*.md") if p.name[:1].isdigit()])
    problems: list[str] = []
    for m in matches:
        stated = int(m.group(1))
        if stated != real:
            problems.append(
                f"docs/README.md:{_line_no(text, m.start())}: states "
                f"{stated} ADRs, docs/adr/ has {real} numbered ADR file(s)")
    return problems


def find_chip_library_count_drift(root: Path) -> list[str]:
    doc = root / "docs" / "contributing-tier-2.md"
    if not doc.is_file():
        return ["docs/contributing-tier-2.md: not found -- cannot verify the "
                 "chip/library counts"]
    text = doc.read_text(encoding="utf-8")
    matches = list(_CHIP_LIBRARY_COUNT_RE.finditer(text))
    if not matches:
        return ["docs/contributing-tier-2.md: could not find the 'N chips, M "
                 "libraries' line -- update this gate's anchor if the wording "
                 "changed"]

    problems: list[str] = []
    chips_dir = root / "chips"
    libraries_dir = root / "metadata" / "libraries"
    if not chips_dir.is_dir():
        problems.append("chips: not found -- cannot verify the chip count")
    if not libraries_dir.is_dir():
        problems.append("metadata/libraries: not found -- cannot verify the "
                         "library count")
    if not chips_dir.is_dir() or not libraries_dir.is_dir():
        return problems

    real_chips = len([p for p in chips_dir.iterdir() if p.is_dir()])
    real_libs = len(list(libraries_dir.glob("*.yaml")))
    for m in matches:
        stated_chips, stated_libs = int(m.group(1)), int(m.group(2))
        line_no = _line_no(text, m.start())
        if real_chips != stated_chips:
            problems.append(
                f"docs/contributing-tier-2.md:{line_no}: states "
                f"{stated_chips} chips, chips/ has {real_chips} "
                f"director{'y' if real_chips == 1 else 'ies'}")
        if real_libs != stated_libs:
            problems.append(
                f"docs/contributing-tier-2.md:{line_no}: states "
                f"{stated_libs} libraries, metadata/libraries/ has "
                f"{real_libs} *.yaml file(s)")
    return problems


def find_problems(root: Path) -> list[str]:
    return (
        find_workflow_count_drift(root)
        + find_workflow_link_drift(root)
        + find_adr_count_drift(root)
        + find_chip_library_count_drift(root)
    )


def main() -> int:
    problems = find_problems(ROOT)
    if problems:
        print("check-doc-inventory: doc-stated workflow/ADR/chip/library "
              "counts or names don't match the tree:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        print(f"\ncheck-doc-inventory: {len(problems)} problem(s) -- failing.",
              file=sys.stderr)
        return 1
    print("check-doc-inventory: OK (workflow count/names, ADR count, chip/library "
          "counts all match the tree).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
