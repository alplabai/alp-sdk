#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""Verify every `path:line` citation in `changelog.d/` still resolves.

WHY THIS EXISTS
---------------
Release notes cite source locations, and source moves. On 2026-08-13 the same
citation in `changelog.d/1387.md` was wrong TWICE in one day: it pointed at
`cc3501e_hw_ti_wifi.c:666-668` (which was the unrelated ASSOCIATION_REJECTED
block), was corrected to `:682-684`, and then a later commit on the same branch
shifted the file again so `:682-684` became the unrelated "GATE CONNECTED ON
L3-UP" comment. Twice, in a shipped, customer-facing release note, with a human
and two adversarial reviewers in the loop.

"Re-check your citations" had already been said and had already failed. So this
is a gate.

WHAT IT CATCHES, AND WHAT IT DOES NOT -- read this before trusting it
--------------------------------------------------------------------
CATCHES, hard failure:
  * a cited path that does not exist in the tree;
  * a cited line number past the end of the file.

CATCHES, only when the fragment opts in (see ANCHORS below):
  * SEMANTIC ROT -- the line still exists but no longer says what the note
    claims. **This is the #1387 case, and the existence/range checks alone would
    NOT have caught it**, because `:682-684` remained a perfectly valid range
    the whole time. Stating that plainly because a gate whose limits are not
    written down gets trusted for things it cannot do -- which is the same class
    of defect this file exists to fight.

DOES NOT CATCH:
  * a citation into another repository (e.g. an alp-sdk fragment citing
    `python/tan/...` or `python/tests/...`, both tan-cli). Any path rooted at
    one of `_FOREIGN_PREFIXES`'s top-level directories is skipped, not just
    the specific subpath in this example -- alp-sdk has no `python/`,
    `crates/`, or `contract/` directory of its own, so the whole subtree is
    unresolvable here regardless of which subpath is cited. Those are
    reported as SKIPPED with a reason, never as a silent pass.

ANCHORS -- how to make a citation checkable
-------------------------------------------
Follow the citation with a short verbatim quote from the cited region:

    the hazard note at `chips/cc3501e/x.c:682-684` ("Do NOT read the RSSI here")

The gate then requires that text to appear within the cited range. Move the
code and the gate fails, which is the entire point. An un-anchored citation is
range-checked only, and is reported so the count of unanchored citations is
visible rather than assumed to be zero.

Exit codes:
    0  every citation resolved (and every anchored one matched)
    1  at least one citation is broken
    2  usage / environment error
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


def _repo_root() -> Path:
    """Asked of git rather than derived from __file__, which breaks when this
    script is invoked from a copy."""
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        raise SystemExit("error: not inside a git worktree")
    return Path(out)


REPO = _repo_root()
FRAGMENT_DIR = REPO / "changelog.d"

#: `path/to/file.ext:123` or `path/to/file.ext:123-456`, inside backticks.
_CITATION = re.compile(
    r"`(?P<path>\.?[A-Za-z0-9_][A-Za-z0-9_/.+-]*\.(?:c|h|cpp|hpp|py|sh|ya?ml|md|json|bb|bbappend|cmake|txt|dts|dtsi|overlay|conf))"
    r":(?P<start>\d+)(?:-(?P<end>\d+))?`"
)

#: An optional anchor: a parenthesised backtick- or quote-delimited phrase
#: immediately following the citation.
_ANCHOR = re.compile(r"""^\s*\(\s*["“`](?P<text>[^"”`]{4,120})["”`]""")

#: Top-level directories that belong to a different repository, in full --
#: not a hand-picked list of subpaths within them. alp-sdk has no `python/`,
#: `crates/`, or `contract/` directory of its own (verified: `ls -d python
#: crates contract` all fail here), so ANY path rooted at one of these is
#: unresolvable in this tree regardless of which subpath is cited --
#: `python/tan/...` and `python/tests/...` are equally foreign. An earlier
#: version of this list matched only `python/tan/` and hard-failed the
#: equally-foreign `python/tests/...` (alp-sdk#1522, alp-sdk#1525). Reported
#: as SKIPPED with the reason, never silently passed.
_FOREIGN_PREFIXES = ("python/", "crates/", "contract/")


#: `CHANGELOG.md` is checked too, but NOT uniformly (alp-sdk#1715). A citation
#: was previously verified only while it sat in a `changelog.d/` fragment and
#: stopped being checked the moment that fragment was folded into
#: `CHANGELOG.md` at release time -- precisely when it starts to rot. Scanning
#: the released file closes that hole, but it cannot carry the same verdict:
#:
#:   * The `[Unreleased]` section is content still being written for a release
#:     that has not shipped. A citation there is cheap to fix and SHOULD be
#:     correct against `HEAD`, so it HARD-FAILS exactly like a fragment.
#:   * Everything below it is the shipped historical record. Those entries
#:     describe the tree as it stood at THAT release; ordinary refactoring moves
#:     the lines they cite, through no fault of the entry. Hard-failing them
#:     would redden this gate for every unrelated PR that moves a cited line,
#:     and "fixing" one by re-pointing it at today's code silently changes what
#:     the entry historically claimed. They are reported as WARNINGS: visible,
#:     actionable, and never blocking.
CHANGELOG = REPO / "CHANGELOG.md"

#: The `[Unreleased]` section: its heading through to the next `## [` heading.
_UNRELEASED = re.compile(r"^## \[Unreleased\].*?(?=^## \[)", re.S | re.M)


def _split_changelog(text: str) -> tuple[str, str]:
    """Split `CHANGELOG.md` into (unreleased section, shipped history).

    A `CHANGELOG.md` with no `[Unreleased]` heading is treated as entirely
    historical -- the conservative reading, since the alternative would
    hard-fail the whole shipped record.
    """
    m = _UNRELEASED.search(text)
    if not m:
        return "", text
    return m.group(0), text[:m.start()] + text[m.end():]


def _iter_fragments() -> list[Path]:
    if not FRAGMENT_DIR.is_dir():
        return []
    return sorted(p for p in FRAGMENT_DIR.glob("*.md") if p.name != "README.md")


def _strip_fenced_blocks(text: str) -> str:
    """Blank out ``` fenced blocks, preserving line count and offsets.

    A fenced block is markdown's display construct: it is where a fragment puts
    an EXAMPLE of a citation rather than a citation. This gate's own fragment
    was the first case -- it demonstrates the anchor syntax with a made-up
    `chips/cc3501e/x.c:682-684`, and an earlier version of this script flagged
    that as a broken citation, i.e. it went red on its own documentation. A gate
    that cannot be written about is a gate people route around.

    Replaced with spaces rather than deleted so every reported line number still
    refers to the real line in the file.
    """
    out, fenced = [], False
    for line in text.split("\n"):
        if line.lstrip().startswith("```"):
            fenced = not fenced
            out.append(" " * len(line))
            continue
        out.append(" " * len(line) if fenced else line)
    return "\n".join(out)


def _check_one(frag: Path, text: str) -> tuple[list[str], list[str], int, int]:
    """Return (errors, skips, checked, anchored) for one fragment."""
    errors: list[str] = []
    skips: list[str] = []
    checked = anchored = 0

    text = _strip_fenced_blocks(text)

    for m in _CITATION.finditer(text):
        rel, start = m.group("path"), int(m.group("start"))
        end = int(m.group("end") or start)
        where = f"{frag.name}: `{rel}:{m.group('start')}" + (
            f"-{m.group('end')}`" if m.group("end") else "`")

        if rel.startswith(_FOREIGN_PREFIXES):
            skips.append(f"{where} -- path belongs to another repository; "
                         f"not checkable here")
            continue

        target = REPO / rel
        if not target.is_file():
            errors.append(f"{where} -- no such file in this tree")
            continue

        lines = target.read_text(encoding="utf-8", errors="replace").splitlines()
        if end > len(lines):
            errors.append(f"{where} -- file has only {len(lines)} lines")
            continue

        checked += 1

        anchor = _ANCHOR.match(text[m.end():])
        if not anchor:
            continue
        anchored += 1
        needle = anchor.group("text").strip()
        region = "\n".join(lines[start - 1:end])
        if needle not in region:
            errors.append(
                f"{where} -- anchored on {needle!r}, but that text is not in "
                f"the cited range. The code moved; re-resolve the citation "
                f"instead of widening the range."
            )

    return errors, skips, checked, anchored


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    fragments = _iter_fragments()
    if not fragments and not CHANGELOG.is_file():
        print("check-changelog-citations: no changelog.d/ fragments and no "
              "CHANGELOG.md -- nothing to check. This is not a pass; it means "
              "there is nothing here to read.")
        return 0
    if not fragments:
        print("check-changelog-citations: no changelog.d/ fragments -- "
              "checking CHANGELOG.md only.")

    all_errors: list[str] = []
    all_skips: list[str] = []
    total_checked = total_anchored = 0

    for frag in fragments:
        errs, skips, checked, anchored = _check_one(
            frag, frag.read_text(encoding="utf-8", errors="replace"))
        all_errors += errs
        all_skips += skips
        total_checked += checked
        total_anchored += anchored

    # CHANGELOG.md: `[Unreleased]` hard-fails, shipped history warns. See the
    # comment above CHANGELOG for why the two halves cannot share a verdict.
    changelog_warnings: list[str] = []
    if CHANGELOG.is_file():
        unreleased, history = _split_changelog(
            CHANGELOG.read_text(encoding="utf-8", errors="replace"))
        errs, skips, checked, anchored = _check_one(CHANGELOG, unreleased)
        all_errors += errs
        all_skips += skips
        total_checked += checked
        total_anchored += anchored
        warns, wskips, wchecked, wanchored = _check_one(CHANGELOG, history)
        changelog_warnings += warns
        all_skips += wskips
        total_checked += wchecked
        total_anchored += wanchored

    for s in all_skips:
        print(f"  SKIP {s}")

    if all_errors:
        print(f"\ncheck-changelog-citations: {len(all_errors)} broken "
              f"citation(s) across {len(fragments)} fragment(s)"
              + (" + CHANGELOG.md's [Unreleased] section"
                 if CHANGELOG.is_file() else "") + ":",
              file=sys.stderr)
        for e in all_errors:
            print(f"  {e}", file=sys.stderr)
        print("\nA release note that cites a line saying something else is "
              "worse than one\nthat cites nothing: it sends the next reader "
              "somewhere confidently wrong.",
              file=sys.stderr)
        return 1

    if changelog_warnings:
        print(f"\ncheck-changelog-citations: {len(changelog_warnings)} stale "
              f"citation(s) in CHANGELOG.md's shipped history (WARNING, not a "
              f"failure -- those entries describe the tree as it stood at their "
              f"release):")
        for w in changelog_warnings:
            print(f"  WARN {w}")

    unanchored = total_checked - total_anchored
    scanned = f"{len(fragments)} fragment(s)" + (
        " + CHANGELOG.md" if CHANGELOG.is_file() else "")
    print(f"check-changelog-citations: OK -- {total_checked} citation(s) "
          f"resolved across {scanned} "
          f"({total_anchored} anchored and text-verified, {unanchored} "
          f"range-checked only"
          + (f", {len(all_skips)} skipped" if all_skips else "") + ").")
    if unanchored and args.verbose:
        print("  note: an un-anchored citation is only checked for existence "
              "and range.\n  Add a quoted anchor -- `path:12-14` (\"the exact "
              "text\") -- to make it\n  fail when the code moves.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
