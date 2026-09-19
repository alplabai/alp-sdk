#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: every in-tree copy of `cc3501e_bridge.{c,h}` under `examples/**` is
byte-identical to the canonical pair, except the copies this gate declares
divergent by name.

Issue #2163: the pair is copied per example -- 16 copies when this gate landed
-- with nothing asserting they stay in sync.  It is not ordinary boilerplate.
`cc3501e_bridge.c` is executable hardware-sequencing code: raw LP-GPIO pad
register writes for `WIFI_EN` / nRESET, the reset pulse timing, and the
silicon-tuned SPI `RX_SAMPLE_DLY` poke.  A silicon fix applied to one copy and
missed in the others is silent in CI and shows up only on hardware.

#1859 is the direct precedent: `cc3501e_gpio_routes.c` was hand-copied per app,
one copy drifted to a wrong `hw_rev` pad map, and the fix was to generate it
from one source.

This gate detects drift rather than removing the duplication, because two
documented properties depend on the copies staying copies:

  - `cc3501e_bridge.h`'s own doc comment presents the pair as a customer-facing
    template ("To reuse in your own AEN application: copy this pair
    (cc3501e_bridge.{c,h}) into your app");
  - `tan init --from-example <dir>` copies exactly one directory, so each
    example has to stay self-contained.  A shared `examples/aen/common/` helper
    would break that flow.

A deliberately different copy is not drift, so `DIVERGENT_COPIES` below names
each one with the reason.  That list is checked in both directions: an entry
naming a directory that carries no copy fails, and so does an entry whose copy
has become byte-identical to the canonical pair again -- an exemption nobody
removes is how the next drift hides.

Run locally:

    python3 scripts/check_cc3501e_bridge_copies.py
    python3 scripts/check_cc3501e_bridge_copies.py --fix   # re-sync the copies

CI wires this in pr-metadata-validate.yml.
"""
from __future__ import annotations

import argparse
import difflib
import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# The template the other copies follow.  aen-cc3501e-bringup is the SoM
# bring-up example the other AEN board.yaml files already cite as the shape to
# match ("Matches examples/aen/aen-cc3501e-bringup/board.yaml").
CANONICAL_DIR = "examples/aen/aen-cc3501e-bringup/src"

# Both halves travel together: a directory carrying one without the other is a
# half-finished copy, which is its own defect.
BRIDGE_FILES = ("cc3501e_bridge.c", "cc3501e_bridge.h")

# Copies that are deliberately NOT the canonical pair, each with the reason it
# differs.  Keep this list short: every entry is a place drift cannot be seen.
DIVERGENT_COPIES = {
    "examples/peripheral-io/alp-console/src": (
        "the console app's bring-up retries cc3501e_hard_reset() in a "
        "cold-boot soak instead of returning the first cc3501e_reset() "
        "status, and omits the RX_SAMPLE_DLY poke (issue #2163)"
    ),
}

_DIFF_CONTEXT_LINES = 3
_MAX_DIFF_LINES = 20


def _rel(path: Path, root: Path) -> str:
    return path.relative_to(root).as_posix()


def _short_diff(canonical: Path, copy: Path, root: Path) -> list[str]:
    """First few differing lines, so the report names the drift, not just the file."""
    diff = difflib.unified_diff(
        canonical.read_text(encoding="utf-8", errors="replace").splitlines(),
        copy.read_text(encoding="utf-8", errors="replace").splitlines(),
        fromfile=_rel(canonical, root),
        tofile=_rel(copy, root),
        n=_DIFF_CONTEXT_LINES,
        lineterm="",
    )
    lines = list(diff)[:_MAX_DIFF_LINES]
    if len(lines) == _MAX_DIFF_LINES:
        lines.append("    ... (truncated; run the diff yourself for the rest)")
    return lines


def _copy_dirs(root: Path) -> dict[str, set[str]]:
    """Every examples/ directory holding at least one half of the pair."""
    found: dict[str, set[str]] = {}
    examples = root / "examples"
    if not examples.is_dir():
        return found
    for name in BRIDGE_FILES:
        for path in examples.rglob(name):
            found.setdefault(_rel(path.parent, root), set()).add(name)
    return found


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    dirs = _copy_dirs(root)

    canonical_dir = root / CANONICAL_DIR
    missing_canonical = [n for n in BRIDGE_FILES if not (canonical_dir / n).is_file()]
    if missing_canonical:
        # Without the template there is nothing to compare against, and a
        # silently skipped gate is worse than a loud one.
        problems.append(
            f"{CANONICAL_DIR}: canonical bridge pair is incomplete -- missing "
            f"{', '.join(missing_canonical)}. Every other copy is graded against "
            f"it, so this gate cannot assert anything until it is restored "
            f"(issue #2163)"
        )
        return problems

    for rel_dir in sorted(dirs):
        if rel_dir == CANONICAL_DIR:
            continue
        present = dirs[rel_dir]
        missing = [n for n in BRIDGE_FILES if n not in present]
        if missing:
            problems.append(
                f"{rel_dir}: carries {', '.join(sorted(present))} without "
                f"{', '.join(missing)} -- the pair is copied together or not at "
                f"all (issue #2163)"
            )
            continue

        identical = all(
            (root / rel_dir / n).read_bytes() == (canonical_dir / n).read_bytes()
            for n in BRIDGE_FILES
        )

        if rel_dir in DIVERGENT_COPIES:
            if identical:
                problems.append(
                    f"{rel_dir}: declared divergent in "
                    f"scripts/check_cc3501e_bridge_copies.py's DIVERGENT_COPIES, "
                    f"but both files now match {CANONICAL_DIR} byte for byte. "
                    f"Drop the entry so this copy is graded with the rest -- a "
                    f"stale exemption is where the next drift hides (issue #2163)"
                )
            continue

        for name in BRIDGE_FILES:
            copy = root / rel_dir / name
            canonical = canonical_dir / name
            if copy.read_bytes() == canonical.read_bytes():
                continue
            detail = "\n".join(f"    {line}" for line in _short_diff(canonical, copy, root))
            problems.append(
                f"{rel_dir}/{name}: differs from {CANONICAL_DIR}/{name}. This "
                f"file is hardware-sequencing code copied per example; a fix "
                f"that lands in one copy and not the others is silent until it "
                f"reaches silicon (issue #2163). Re-sync with\n"
                f"    cp {CANONICAL_DIR}/{name} {rel_dir}/{name}\n"
                f"or, if the difference is deliberate, declare it in "
                f"DIVERGENT_COPIES with the reason.\n{detail}"
            )

    for rel_dir in sorted(DIVERGENT_COPIES):
        if rel_dir not in dirs:
            problems.append(
                f"{rel_dir}: named in DIVERGENT_COPIES but carries no "
                f"cc3501e_bridge copy. Drop the stale entry (issue #2163)"
            )

    return problems


def resync(root: Path) -> list[str]:
    """Overwrite every non-divergent copy with the canonical pair."""
    canonical_dir = root / CANONICAL_DIR
    fixed: list[str] = []
    for rel_dir in sorted(_copy_dirs(root)):
        if rel_dir == CANONICAL_DIR or rel_dir in DIVERGENT_COPIES:
            continue
        for name in BRIDGE_FILES:
            copy = root / rel_dir / name
            canonical = canonical_dir / name
            if not canonical.is_file() or not copy.is_file():
                continue
            if copy.read_bytes() == canonical.read_bytes():
                continue
            shutil.copyfile(canonical, copy)
            fixed.append(f"{rel_dir}/{name}")
    return fixed


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO,
                    help="repo root to scan (default: the real repo)")
    ap.add_argument("--fix", action="store_true",
                    help="copy the canonical pair over every non-divergent copy "
                         "that differs, then re-check")
    args = ap.parse_args()

    if args.fix:
        for rel in resync(args.root):
            print(f"re-synced {rel}")

    problems = find_problems(args.root)
    if problems:
        print("check_cc3501e_bridge_copies: found problems:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1

    copies = [d for d in _copy_dirs(args.root) if d != CANONICAL_DIR]
    graded = [d for d in copies if d not in DIVERGENT_COPIES]
    print(f"OK: {len(graded)} cc3501e_bridge copies byte-identical to "
          f"{CANONICAL_DIR}, {len(DIVERGENT_COPIES)} declared divergent.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
