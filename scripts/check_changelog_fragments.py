#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Guard: every `changelog.d/*.md` fragment is well-formed (#1395).

`changelog.d/` replaces the single `CHANGELOG.md` insertion point that made
every open PR conflict with every other one on the same lines (alp-sdk#1395).
That only works if a fragment is guaranteed foldable at release time by
`scripts/assemble_changelog.py` -- a malformed fragment left undetected until
the release PR is exactly the "silently dropped changelog entry" failure this
system exists to prevent, just deferred to the worst possible moment.

This gate fails a PR the moment it adds a fragment that would be REFUSED by
the assembler:

  1. the filename isn't `<issue>.md` or `<issue>-<slug>.md` -- issue digits
     first, so the number stays the greppable join key even when a slug
     disambiguates a second fragment for the same issue (`changelog.d/README.md`
     is exempt from this rule);
  2. the file is empty (or whitespace-only);
  3. the file doesn't start with its own `### <Category> - <Title>` heading
     line -- alp-sdk's format gives every entry its own heading rather than
     bucketing into six fixed Keep-a-Changelog lists, so this is the one
     structural requirement a fragment has (see changelog.d/README.md).

Deliberately does NOT validate `<Category>` against a fixed list: unlike
tan-cli's assembler (tan-cli#676), alp-sdk keeps no category enum to drift
out of sync with real usage (measured 8 distinct category words already in
use under `## [Unreleased]`, wider than Keep a Changelog's six).
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

sys.path.insert(0, str(REPO / "scripts"))
import assemble_changelog as ac  # noqa: E402


def find_problems(root: Path) -> list[str]:
    frag_dir = root / "changelog.d"
    if not frag_dir.is_dir():
        return [f"{frag_dir.relative_to(root)} is missing"]

    try:
        ac.load_fragments(frag_dir)
    except ac.AssembleError as exc:
        return [str(exc)]
    return []


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO,
                     help="repo root to scan (default: the real repo)")
    args = ap.parse_args()

    problems = find_problems(args.root)
    if problems:
        for p in problems:
            print(f"changelog-fragments: {p}", file=sys.stderr)
        return 1
    print("OK: every changelog.d/ fragment is well-formed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
