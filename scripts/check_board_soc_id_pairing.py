#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: every mention of an upstream Alif Zephyr dev-kit board target
must pair the board with ITS OWN SoC id -- never a different AEN family
member's.

Issue #1266: docs and example READMEs repeatedly wrote
`ensemble_e8_dk/ae402fa0e5597le0/rtss_hp` -- the upstream `ensemble_e8_dk`
board (Alif's own E8 dev kit, the exact E1M-AEN801 part) paired with
`ae402fa0e5597le0`, which is the E1M-AEN401 (Alif Ensemble **E4**)'s SoC
id (`zephyr/boards/alp/e1m_aen401_m55_hp/board.yml`), not the E8's own
`ae822fa0e5597ls0` (`zephyr/boards/alp/e1m_aen801_m55_hp/board.yml`,
`docs/bring-up-aen.md`'s "Upstream known-good fallback" paragraph). A
customer who copy-pastes that `west build -b ...` line onto a real E8 dev
kit hits a board/SoC mismatch. The mistake reached 29 files across
`docs/superpowers/plans/*.md` and `examples/*/README.md` before a manual
accuracy sweep caught it -- nothing in CI keyed on the board/SoC pairing,
so it could recur silently. This gate closes that gap.

`_KNOWN_BOARD_SOC_IDS` is the allowlist of upstream board -> correct SoC
id pairs this gate knows about. Add an entry if another upstream Alif
dev-kit board target picks up the same copy/paste habit.

`CHANGELOG.md` is excluded -- it is history, not a live claim, the same
scope issue #1266's own "Full instance list" used.

Run locally:

    python3 scripts/check_board_soc_id_pairing.py
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Upstream Zephyr board name -> the ONLY SoC id it may be paired with in
# a `<board>/<soc_id>/<core>` target string appearing in prose or config.
_KNOWN_BOARD_SOC_IDS: dict[str, str] = {
    "ensemble_e8_dk": "ae822fa0e5597ls0",
}

_TARGET_RE = re.compile(
    r"\b(" + "|".join(re.escape(b) for b in _KNOWN_BOARD_SOC_IDS) + r")/([0-9a-zA-Z]+)\b"
)

# repo-relative path -> reason. CHANGELOG.md is history (issue #1266's own
# instance list explicitly excludes it), not a live claim to fix.
ALLOWLIST: dict[str, str] = {
    "CHANGELOG.md": "history, not a live claim (issue #1266)",
}


def _scanned_files(root: Path) -> list[Path]:
    """docs/**, examples/**/README.md + testcase.yaml + C sources
    (a `src/main.c` build-matrix teaching comment carries the same
    board/SoC target string a README does --
    examples/display/lvgl-widgets-demo/src/main.c did, in this issue),
    tests/hil/**/*.md (the HIL per-board target table), .github/
    workflows/** (a CI matrix step can name the same target), and the
    template/example catalogs."""
    out: list[Path] = []
    docs = root / "docs"
    if docs.is_dir():
        out.extend(sorted(docs.rglob("*.md")))
    examples = root / "examples"
    if examples.is_dir():
        out.extend(sorted(examples.rglob("README.md")))
        out.extend(sorted(examples.rglob("testcase.yaml")))
        out.extend(sorted(examples.rglob("*.c")))
        out.extend(sorted(examples.rglob("*.h")))
    hil = root / "tests" / "hil"
    if hil.is_dir():
        out.extend(sorted(hil.rglob("*.md")))
    workflows = root / ".github" / "workflows"
    if workflows.is_dir():
        out.extend(sorted(workflows.rglob("*.yml")))
        out.extend(sorted(workflows.rglob("*.yaml")))
    for rel in ("metadata/catalog.json", "metadata/templates/catalog-v1.json"):
        p = root / rel
        if p.is_file():
            out.append(p)
    changelog = root / "CHANGELOG.md"
    if changelog.is_file():
        out.append(changelog)
    return out


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    for path in _scanned_files(root):
        rel = path.relative_to(root).as_posix()
        if rel in ALLOWLIST:
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for i, line in enumerate(text.splitlines(), start=1):
            for m in _TARGET_RE.finditer(line):
                board, soc_id = m.group(1), m.group(2)
                expected = _KNOWN_BOARD_SOC_IDS[board]
                if soc_id != expected:
                    problems.append(
                        f"{rel}:{i}: '{board}/{soc_id}' pairs {board} with "
                        f"the wrong SoC id -- expected '{expected}': "
                        f"{line.strip()[:120]}"
                    )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    problems = find_problems(args.root)
    if problems:
        print("check_board_soc_id_pairing: found problems:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1
    print("OK: every known upstream board target pairs its own SoC id.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
