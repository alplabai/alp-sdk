#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Reject the unlocked check-then-set slot claim (issue #1630).

A static handle pool must claim its slot with ``alp_slot_try_claim()``
(``src/common/alp_slot_claim.h``), whose compare-exchange lets exactly one
concurrent opener win.  A plain ``if (!in_use) { in_use = true; }`` is two
operations: two openers can both read false, both take the slot, and alias
their handles, so one caller's ``close()`` frees state the other still holds.

This class survived BOTH #1115 and #629 because each was remediated from a
hand-written site list rather than from a grep.  This gate is what makes "did
we get them all" a CI answer instead of a judgement call.

**What it looks for, and why that half.**  The gate flags the *set* half --
any plain assignment of a ``*in_use*`` flag to ``true`` -- rather than the
``if (!in_use)`` test half.  Two reasons, and the second is the whole point of
this issue:

  * ``alp_slot_try_claim()`` never assigns the flag, so a converted site has no
    such assignment.  The set half is therefore a complete signal on its own,
    with no need to guess a line window between a test and its set.
  * The test half is not always a negation.  ``src/backends/inference/tflm.cpp``
    guarded a bare singleton with ``if (g_default_arena_in_use) { fail; }`` --
    positive, no loop, no subscript -- and every array-shaped grep in #1115's
    remediation walked straight past it.  Matching the assignment catches the
    array shape and the singleton shape with one rule.

Comments are stripped before scanning: ``src/`` carries a dozen lines that
quote the antipattern while explaining why the code below no longer does it.

Allowlisted sites are already serialised by a held lock, so a compare-exchange
there would be pure churn.  Each entry must name the lock, so the list cannot
grow by assertion -- widening it to make the gate green is the exact failure
mode this gate exists to prevent.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# Repo-relative path -> the lock that already serialises the claim.
ALLOWLIST: dict[str, str] = {
    "src/zephyr/handles.c": (
        "the whole scan runs inside k_mutex_lock(&kind##_lock, K_FOREVER)"
    ),
    "src/yocto/peripheral_gpio.c": "inside pthread_mutex_lock(&g_irq.mu)",
    "src/backends/can/yocto_drv.c": "inside pthread_mutex_lock(&d->lock)",
    "src/backends/can/testing_drv.c": (
        "a per-handle filter table reached through one handle, not a shared "
        "static pool"
    ),
}

SUFFIXES = (".c", ".cpp", ".h", ".hpp")

# `<anything>in_use<anything> = true` -- the set half of the antipattern.
_SET = re.compile(r"[A-Za-z0-9_.\[\]>-]*in_use[A-Za-z0-9_.\[\]>-]*\s*=\s*true\b")

# Block and line comments, plus string/char literals so a comment opener inside
# a literal cannot swallow the rest of the file.
_COMMENT_OR_LITERAL = re.compile(
    r"/\*.*?\*/|//[^\n]*|\"(?:\\.|[^\"\\\n])*\"|'(?:\\.|[^'\\\n])*'",
    re.DOTALL,
)


def _blank_comments(text: str) -> str:
    """Blank out comments and literals, keeping every newline in place.

    Line numbers must survive so a report points at the real line.
    """
    return _COMMENT_OR_LITERAL.sub(lambda m: "\n" * m.group(0).count("\n"), text)


def find_problems(root: Path) -> list[str]:
    """Return one message per unlocked check-then-set slot claim under src/."""
    problems: list[str] = []
    src = root / "src"
    if not src.is_dir():
        return problems
    for path in sorted(src.rglob("*")):
        if path.suffix not in SUFFIXES or not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        if rel in ALLOWLIST:
            continue
        text = _blank_comments(path.read_text(encoding="utf-8", errors="replace"))
        for n, line in enumerate(text.splitlines(), start=1):
            if _SET.search(line):
                problems.append(
                    f"{rel}:{n}: a slot's in_use flag is assigned directly; claim "
                    f"it with alp_slot_try_claim() from src/common/alp_slot_claim.h "
                    f"instead (issue #1630) -- a plain check-then-set lets two "
                    f"concurrent openers win the same slot and alias their handles. "
                    f"If this site is already serialised by a held lock, add it to "
                    f"ALLOWLIST in {Path(__file__).name} with the lock named."
                )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description="Check atomic slot claims under src/.")
    ap.add_argument("--root", default=".", help="repository root to scan")
    args = ap.parse_args()
    problems = find_problems(Path(args.root))
    for p in problems:
        print(p, file=sys.stderr)
    if problems:
        print(
            f"\n{len(problems)} unlocked slot claim(s) found.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
