#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Fold `changelog.d/` fragments into CHANGELOG.md's `## [Unreleased]` section.

WHY THIS EXISTS
---------------
`CHANGELOG.md` has one insertion point: the top of the
`## [Unreleased] - vX candidate` section. Every open PR appends its entry
there, so any two PRs conflict on it by construction, and the conflict
re-fires on every merge -- land one PR and the rest go dirty again. Measured
2026-08-12 (alp-sdk#1395): three of four PRs blocked at the time were blocked
by `CHANGELOG.md` alone, with no other conflicted file, and they conflicted
with *each other* too, forcing a strictly serial, fully-gated land order.

One file per change removes the entire class: disjoint files cannot conflict.
This script is the other half -- it folds the fragments back into the one
document `scripts/bump_version.py` and `.github/workflows/release.yml`
actually slice, so the release contract does not change.

ALP-SDK'S FORMAT DIFFERS FROM TAN-CLI'S (tan-cli#676) -- DO NOT PORT VERBATIM
------------------------------------------------------------------------------
tan-cli buckets fragments into six fixed `### Added` / `### Changed` / ...
lists and splices bullets into the right one. alp-sdk instead gives every
changelog entry its OWN `###` heading -- `### <Category> -- <Title>` --
followed by prose (measured: 47 such headings under the live `## [Unreleased]`
section, across 8 distinct category words used repo-wide -- a wider set than
Keep a Changelog's six, and not worth enumerating here). An alp-sdk fragment
is therefore already a complete, self-contained block, so this assembler does
no bucketing and keeps no category enum in sync -- it only concatenates
fragments, in filename order, at the top of the `## [Unreleased]` section.

WHAT IT DELIBERATELY DOES NOT DO
---------------------------------
It does not reformat, rewrap, summarise, or reorder text INSIDE a fragment.
This changelog carries registers, hex, bit fields, addresses, SKUs, hw_rev,
diagnostic codes, error strings and paths verbatim, and a rewrap can silently
corrupt one of those. Fragment content is copied byte-for-byte.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

FRAGMENT_NAME_RE = re.compile(r"^\d+\.md$")
UNRELEASED_PREFIX = "## [Unreleased]"
SECTION_PREFIX = "## ["


class AssembleError(RuntimeError):
    """A condition that must stop the release, not be worked around."""


def repo_root(start: Path) -> Path:
    """Walk up to the directory holding both CHANGELOG.md and changelog.d/."""
    for candidate in (start, *start.parents):
        if (candidate / "CHANGELOG.md").is_file() and (candidate / "changelog.d").is_dir():
            return candidate
    raise AssembleError(
        "could not locate a directory containing both CHANGELOG.md and "
        f"changelog.d/, starting from {start}"
    )


def _fragment_sort_key(path: Path) -> tuple[int, object]:
    """Numeric order by issue number where possible, so `2.md` sorts before
    `10.md` -- lexicographic sort would put them the other way round."""
    stem = path.stem
    if stem.isdigit():
        return (0, int(stem))
    return (1, stem)


def load_fragments(frag_dir: Path) -> list[tuple[Path, str]]:
    """Return `[(path, body), ...]` for every fragment, in deterministic order.

    Deterministic order is what makes assembly reproducible: the same
    fragment set must produce the same CHANGELOG.md bytes on every machine,
    or every release diff looks like unrelated noise.
    """
    candidates = sorted(
        (p for p in frag_dir.glob("*.md") if p.name != "README.md"),
        key=_fragment_sort_key,
    )

    bad: list[str] = []
    fragments: list[tuple[Path, str]] = []
    for path in candidates:
        if not FRAGMENT_NAME_RE.match(path.name):
            bad.append(f"{path.name} (expected `<issue>.md`, digits only)")
            continue
        body = path.read_text(encoding="utf-8").strip("\n")
        if not body.strip():
            bad.append(f"{path.name} (empty)")
            continue
        if not body.lstrip().startswith("### "):
            bad.append(f"{path.name} (must start with its own `### <Category> - <Title>` heading)")
            continue
        fragments.append((path, body))

    if bad:
        raise AssembleError(
            "unusable changelog.d/ fragment(s): "
            + "; ".join(bad)
            + "\nsee changelog.d/README.md for the contract. Refusing to "
            "continue rather than silently dropping an entry."
        )
    return fragments


def find_unreleased(lines: list[str]) -> tuple[int, int]:
    """Return `(header_index, end_index)` for the `## [Unreleased]` section.

    `end_index` is the index of the next `## [` header, or `len(lines)`.
    """
    start = None
    for i, line in enumerate(lines):
        if line.startswith(UNRELEASED_PREFIX):
            start = i
            break
    if start is None:
        raise AssembleError(
            "CHANGELOG.md has no `## [Unreleased]` header. Refusing to "
            "guess where fragments belong."
        )
    for j in range(start + 1, len(lines)):
        if lines[j].startswith(SECTION_PREFIX):
            return start, j
    return start, len(lines)


def fold(lines: list[str], fragments: list[tuple[Path, str]]) -> list[str]:
    """Insert every fragment body at the TOP of `## [Unreleased]`, in order.

    Existing hand-written entries already in the section are KEPT, pushed
    below the newly-folded fragments -- nothing already there is overwritten
    or reordered.
    """
    start, end = find_unreleased(lines)
    header = lines[start]
    body = list(lines[start + 1:end])

    # The section's existing body may open with a blank separator line
    # (the normal case) -- drop it so re-inserting doesn't double it up.
    while body and not body[0].strip():
        body.pop(0)

    inserted: list[str] = []
    for _, text in fragments:
        if inserted:
            inserted.append("")
        inserted.extend(text.split("\n"))

    new_section = [header, ""]
    new_section.extend(inserted)
    if inserted and body:
        new_section.append("")
    new_section.extend(body)

    # Exactly one blank line separates the section from whatever follows
    # (the next `## [` heading, or EOF) -- matches the file's existing style.
    while new_section and not new_section[-1].strip():
        new_section.pop()
    new_section.append("")

    return lines[:start] + new_section + lines[end:]


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--check", action="store_true",
                     help="list pending fragments; change nothing; exit 0 always "
                          "(informational -- use --require-empty for a real gate)")
    ap.add_argument("--require-empty", action="store_true",
                     help="exit 1 if any fragment remains unfolded (the release gate)")
    ap.add_argument("--dry-run", action="store_true",
                     help="print the resulting CHANGELOG.md to stdout; write nothing")
    ap.add_argument("--root", type=Path, default=None,
                     help="repo root (default: discovered from this script's location)")
    args = ap.parse_args(argv)

    try:
        root = args.root or repo_root(Path(__file__).resolve().parent)
        frag_dir = root / "changelog.d"
        changelog = root / "CHANGELOG.md"

        fragments = load_fragments(frag_dir)

        if args.check or args.require_empty:
            for path, _ in fragments:
                print(path.name)
            print(f"{len(fragments)} fragment(s) pending")
            if args.require_empty and fragments:
                print(
                    "::error::unfolded changelog.d/ fragments remain -- run "
                    "`python3 scripts/assemble_changelog.py` and commit the "
                    "result before cutting a release",
                    file=sys.stderr,
                )
                return 1
            return 0

        if not fragments:
            print("no changelog.d/ fragments to fold")
            return 0

        lines = changelog.read_text(encoding="utf-8").splitlines()
        merged = fold(lines, fragments)
        text = "\n".join(merged).rstrip("\n") + "\n"

        if args.dry_run:
            sys.stdout.write(text)
            return 0

        changelog.write_text(text, encoding="utf-8", newline="")
        for path, _ in fragments:
            path.unlink()
        print(f"folded {len(fragments)} fragment(s) into {changelog.relative_to(root)}")
        return 0

    except AssembleError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
