#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Reject a fixed-literal `alp_delay_us(N)` with N >= 1000 under chips/.

`alp_delay_us()` is documented (`include/alp/peripheral.h`) as a
non-yielding busy-wait -- Zephyr's backend (`src/zephyr/delay_zephyr.c`)
routes it straight to `k_busy_wait()`, which never releases the CPU to
other threads. It is scoped to sub-millisecond hardware-timing sequences
only; `alp_delay_ms()` (-> `k_msleep()`, which yields) is the contract for
anything a millisecond or longer.

Issue #1621: 34 chip-driver call sites passed a fixed literal >= 1000 (up
to `alp_delay_us(1500000)`, a 1.5 s spin in
`chips/ublox_sara_r5/ublox_sara_r5.c`) through the non-yielding primitive,
stalling every equal-or-lower-priority thread on that core for the whole
window -- long enough to starve a watchdog-feed thread. This gate is a
grep-grade textual check (matching this repo's other regex-based
call-site gates, e.g. check_apt_bounded.py): it only catches a *fixed
integer literal* argument, so it does not (and should not) flag a
variable/macro/expression argument -- those are caller- or
config-controlled and are reviewed case by case, not fixed in bulk here.

Run locally:

    python3 scripts/check_delay_us_literal.py
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

# A call whose sole argument is an integer literal (optional digit-group
# separators, optional u/U/l/L suffix) -- NOT a variable, macro, or
# expression. `\b` keeps this from matching inside a longer identifier.
_CALL_RE = re.compile(r"\balp_delay_us\(\s*(\d[\d']*)\s*[uUlL]*\s*\)")
_THRESHOLD_US = 1000


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    chips_dir = root / "chips"
    if not chips_dir.is_dir():
        return problems

    for c_path in sorted(chips_dir.rglob("*.c")):
        rel = c_path.relative_to(root)
        for lineno, line in enumerate(
            c_path.read_text(encoding="utf-8").splitlines(), start=1
        ):
            stripped = line.strip()
            if stripped.startswith("*") or stripped.startswith("//"):
                continue
            m = _CALL_RE.search(line)
            if not m:
                continue
            value = int(m.group(1).replace("'", ""))
            if value < _THRESHOLD_US:
                continue
            problems.append(
                f"{rel}:{lineno}: alp_delay_us({value}) is a non-yielding "
                f">= 1ms spin (#1621) -- use alp_delay_ms({value // 1000}) "
                f"instead so the calling thread yields the CPU"
            )
    return problems


def main() -> int:
    problems = find_problems(ROOT)
    if problems:
        for p in problems:
            print(f"delay-us-literal: {p}", file=sys.stderr)
        return 1
    print("OK: no fixed-literal alp_delay_us(N>=1000) under chips/.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
