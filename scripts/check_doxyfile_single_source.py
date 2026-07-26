#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Guard: no second Doxyfile-shaped heredoc regrows next to
docs/doxygen/Doxyfile.

docs/doxygen/Doxyfile is the single source of the SDK's Doxygen config
(#970): pr-doxygen.yml and scripts/test-all.sh's stage_doxygen both `cat`
it (appending only per-run overrides like PROJECT_NUMBER on stdin)
instead of hand-maintaining their own copy. Two hand-maintained copies
is exactly how INPUT / EXCLUDE_PATTERNS / WARN_AS_ERROR drifted before
#970 -- DOT_GRAPH_MAX_NODES silently absent from one copy turned a real
Doxygen warning into a false CI-red locally, and examples/README.md
silently absent from the other let a real broken link through as a
false local-green. This gate exists so a future edit can't reintroduce
that drift by re-embedding the settings inline.

Detection is a heuristic, not a parser: a handful of Doxygen config tag
names are distinctive enough (they don't occur as YAML/shell keys or
common English words) that seeing several of them assigned in the same
file, outside docs/doxygen/Doxyfile itself, means that file is carrying
its own copy of the Doxyfile rather than referencing the single source.
A single stray mention (e.g. a comment naming one tag) does not trip
this -- the threshold requires multiple distinct tags, the shape of an
actual config block, not a passing reference.

Only fires while docs/doxygen/Doxyfile exists -- if the single source
itself is missing, that is a different problem (the file was deleted)
and not this gate's job to report.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Distinctive Doxygen config tags: unlikely to appear as YAML/shell
# identifiers or ordinary prose, so several of them assigned together
# is a strong signal of a re-embedded Doxyfile rather than coincidence.
_DOXY_TAGS = frozenset({
    "OPTIMIZE_OUTPUT_FOR_C",
    "DOT_GRAPH_MAX_NODES",
    "JAVADOC_AUTOBRIEF",
    "USE_MDFILE_AS_MAINPAGE",
    "WARN_AS_ERROR",
    "EXTRACT_STATIC",
    "GENERATE_LATEX",
})

# Below this many distinct tags in one file, treat it as an incidental
# mention (e.g. a comment or a doc referencing one setting by name)
# rather than a re-embedded config block.
_MIN_DISTINCT_TAGS = 3

_TAG_ASSIGN_RE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*\+?=", re.MULTILINE)

# Heredocs realistically land in a shell script or a workflow's `run:`
# block -- scoping to these extensions keeps this from false-firing on
# unrelated Python/Markdown files that happen to mention a tag name.
_SCAN_GLOBS = (
    (".github/workflows", "*.yml"),
    (".github/workflows", "*.yaml"),
    ("scripts", "*.sh"),
)


def find_problems(root: Path) -> list[str]:
    doxyfile = root / "docs" / "doxygen" / "Doxyfile"
    if not doxyfile.is_file():
        return []

    problems: list[str] = []
    candidates: list[Path] = []
    for subdir, pattern in _SCAN_GLOBS:
        d = root / subdir
        if d.is_dir():
            candidates.extend(d.rglob(pattern))

    for path in sorted(set(candidates)):
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        found = {
            m.group(1)
            for m in _TAG_ASSIGN_RE.finditer(text)
            if m.group(1) in _DOXY_TAGS
        }
        if len(found) >= _MIN_DISTINCT_TAGS:
            rel = path.relative_to(root).as_posix()
            problems.append(
                f"{rel}: looks like a re-embedded Doxyfile ({', '.join(sorted(found))}) "
                f"-- docs/doxygen/Doxyfile is the single source (#970); reference "
                f'it ("cat docs/doxygen/Doxyfile | doxygen -" or "cp ... Doxyfile") '
                f"instead of duplicating its settings"
            )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO,
                     help="repo root to scan (default: the real repo)")
    args = ap.parse_args()

    problems = find_problems(args.root)
    for p in problems:
        print(p, file=sys.stderr)
    if problems:
        print(f"\ncheck_doxyfile_single_source: {len(problems)} site(s) "
              f"re-embed the Doxyfile instead of referencing the single "
              f"source.", file=sys.stderr)
        return 1
    print("OK: no re-embedded Doxyfile found outside docs/doxygen/Doxyfile.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
