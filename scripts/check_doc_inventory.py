#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Four hand-maintained inventory counts/names live in prose and drift
silently whenever the tree they describe changes underneath them (issue
#1265, following up on #1209's fifth required-work item):

  (a) `docs/ci/README.md`'s "carries **N** workflow files" count.
  (b) `docs/ci/README.md`'s "## Workflows shipped" table -- each row links
      `` [`name.yml`](../../.github/workflows/name.yml) ``; the displayed
      name must match the linked file's own name, and the link must
      resolve.
  (c) `docs/README.md`'s "(N ADRs; recount with ...)" docs-index count.
  (d) `docs/contributing-tier-2.md`'s "N chips, M libraries" Tier-1 count.

Scope is deliberately narrower than #1265's initial wording.  An earlier
attempt at this gate (and its sibling #1264) went through three review
rounds without converging: the design was "parse the doc's prose, look for
a section/anchor, skip the check if the anchor isn't found" -- and every
fix closed one fail-open shape (an exemption zone bounded by the wrong
thing, a table-row-only paragraph split that let a bullet list glue
together) while creating another.  This gate avoids that whole bug class
by construction: it checks exactly four ANCHORED, single-fact claims (a
bold count, a `(N ADRs;` parenthetical, an `N chips, M libraries` line, a
`[`file`](path)` link pair) via regex/markdown-link matching, not prose
parsing -- and when an anchor can't be found, that is a hard FAILURE
(`problems.append(...)`), never a silent skip.  See `find_problems()`
below; there is no code path that returns `[]` because a doc was
unreadable or a pattern didn't match.

Deliberately NOT covered (see the postmortem above and #1264's docstring
for the same principle applied to the sibling gate):

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
    extensions even though every workflow in this tree today is .yml and
    the doc's own recount hint (`ls .github/workflows/*.yml | wc -l`) only
    covers one of them."""
    workflows_dir = root / ".github" / "workflows"
    return len(list(workflows_dir.glob("*.yml"))) + len(list(workflows_dir.glob("*.yaml")))


def find_workflow_count_drift(root: Path) -> list[str]:
    doc = root / "docs" / "ci" / "README.md"
    if not doc.is_file():
        return ["docs/ci/README.md: not found -- cannot verify the workflow count"]
    text = doc.read_text(encoding="utf-8")
    m = _WORKFLOW_COUNT_RE.search(text)
    if not m:
        return ["docs/ci/README.md: could not find the 'carries **N** workflow "
                 "files' line -- update this gate's anchor if the wording changed"]
    workflows_dir = root / ".github" / "workflows"
    if not workflows_dir.is_dir():
        return [".github/workflows: not found -- cannot verify the workflow count"]
    stated, real = int(m.group(1)), _real_workflow_count(root)
    if stated != real:
        return [f"docs/ci/README.md: states **{stated}** workflow files, "
                 f".github/workflows/ has {real} (*.yml + *.yaml)"]
    return []


def find_workflow_link_drift(root: Path) -> list[str]:
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
            href_name = Path(href).name
            if href_name != name:
                problems.append(
                    f"docs/ci/README.md:{line_no}: link text `{name}` doesn't "
                    f"match its href's filename ({href_name})")
                continue
            target = (doc.parent / href).resolve()
            if not target.is_file():
                problems.append(
                    f"docs/ci/README.md:{line_no}: links to {href} (`{name}`), "
                    f"which doesn't exist")
    return problems


def find_adr_count_drift(root: Path) -> list[str]:
    doc = root / "docs" / "README.md"
    if not doc.is_file():
        return ["docs/README.md: not found -- cannot verify the ADR count"]
    text = doc.read_text(encoding="utf-8")
    m = _ADR_COUNT_RE.search(text)
    if not m:
        return ["docs/README.md: could not find the '(N ADRs; recount ...)' "
                 "line -- update this gate's anchor if the wording changed"]
    adr_dir = root / "docs" / "adr"
    if not adr_dir.is_dir():
        return ["docs/adr: not found -- cannot verify the ADR count"]
    stated = int(m.group(1))
    # Mirrors the doc's own recount command, `ls docs/adr/[0-9]*.md | wc -l`
    # -- numbered ADR files only, not docs/adr/README.md.
    real = len([p for p in adr_dir.glob("*.md") if p.name[:1].isdigit()])
    if stated != real:
        return [f"docs/README.md: states {stated} ADRs, docs/adr/ has "
                 f"{real} numbered ADR file(s)"]
    return []


def find_chip_library_count_drift(root: Path) -> list[str]:
    doc = root / "docs" / "contributing-tier-2.md"
    if not doc.is_file():
        return ["docs/contributing-tier-2.md: not found -- cannot verify the "
                 "chip/library counts"]
    text = doc.read_text(encoding="utf-8")
    m = _CHIP_LIBRARY_COUNT_RE.search(text)
    if not m:
        return ["docs/contributing-tier-2.md: could not find the 'N chips, M "
                 "libraries' line -- update this gate's anchor if the wording "
                 "changed"]
    stated_chips, stated_libs = int(m.group(1)), int(m.group(2))

    problems: list[str] = []
    chips_dir = root / "chips"
    if not chips_dir.is_dir():
        problems.append("chips: not found -- cannot verify the chip count")
    else:
        real_chips = len([p for p in chips_dir.iterdir() if p.is_dir()])
        if real_chips != stated_chips:
            problems.append(
                f"docs/contributing-tier-2.md: states {stated_chips} chips, "
                f"chips/ has {real_chips} director{'y' if real_chips == 1 else 'ies'}")

    libraries_dir = root / "metadata" / "libraries"
    if not libraries_dir.is_dir():
        problems.append("metadata/libraries: not found -- cannot verify the "
                         "library count")
    else:
        real_libs = len(list(libraries_dir.glob("*.yaml")))
        if real_libs != stated_libs:
            problems.append(
                f"docs/contributing-tier-2.md: states {stated_libs} libraries, "
                f"metadata/libraries/ has {real_libs} *.yaml file(s)")
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
