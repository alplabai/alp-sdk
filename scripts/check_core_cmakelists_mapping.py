#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Core-to-CMakeLists mapping gate: fails (exit 1) when an enabled Zephyr core's
`app:` resolves to no CMakeLists.txt, or when two distinct Zephyr cores
resolve to the same CMakeLists.txt.

The second is the load-bearing one. Each Zephyr core's per-core Kconfig
fragment is written to `<app dir>/generated/alp.conf` (scripts/
gen_example_alp_conf.py; the build plan materialises its own copy), so two
cores sharing one app dir would share ONE fragment slot -- the second core
silently configures with the first core's Kconfig. On a dual-Zephyr-core SKU
(e.g. E1M-AEN801, M55-HE + M55-HP), adding a second `cores.<id>.app:` that
resolves to an ALREADY-WIRED sibling core's app directory is exactly that trap.

(An earlier invariant -- the `--core <id>` literal an example CMakeLists.txt
baked in for `alp_project.py --emit zephyr-conf` must match the core whose
`app:` resolves there -- retired with that configure-time bridge, #866.)

The SDK-owned stock M-core shim (`alp-stock-shim`, firmware/alp-stock-shim/)
is EXCLUDED: every core a board.yaml leaves at its SoM topology default
resolves there BY DESIGN (metadata/e1m_modules/*.yaml
`topology.<id>.app: alp-stock-shim`) -- that is intentional many-to-one
sharing across the whole example corpus, not the drift this gate polices.

Usage:

    python3 scripts/check_core_cmakelists_mapping.py [--root ROOT]
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))

from alp_orchestrate import (  # noqa: E402
    OrchestratorError, iter_buildable_slices, load_board_yaml,
)
from alp_orchestrate.orchestrator import (  # noqa: E402
    STOCK_SHIM_APP, _zephyr_app_dir,
)


def _line_of(text: str, offset: int) -> int:
    """1-based line number of a regex match's start offset."""
    return text.count("\n", 0, offset) + 1


def _core_key_line(board_yaml_text: str, core_id: str) -> int | None:
    """Best-effort 1-based line number of `  <core_id>:` under `cores:` in
    a board.yaml, for a friendlier violation message. None if not found
    (e.g. unusual indentation) -- callers fall back to the bare path."""
    m = re.search(rf"(?m)^  {re.escape(core_id)}:\s*$", board_yaml_text)
    return _line_of(board_yaml_text, m.start()) if m else None


def find_problems(root: Path) -> list[str]:
    """Every literal-agreement (assertion 1) and shared-CMakeLists.txt
    (assertion 2) violation across every `examples/**/board.yaml`."""
    problems: list[str] = []
    # resolved CMakeLists.txt path -> [(core_id, board_yaml path, line)]
    # that resolve there -- gathered across every board.yaml scanned, so a
    # shared file is caught however far apart its cores are declared.
    resolved_by_file: dict[Path, list[tuple[str, Path, int | None]]] = {}

    for board_yaml in sorted(root.glob("examples/**/board.yaml")):
        rel_board = board_yaml.relative_to(root).as_posix()
        try:
            project = load_board_yaml(board_yaml)
        except OrchestratorError as e:
            problems.append(f"{rel_board}: board.yaml failed to load ({e})")
            continue
        base_dir = board_yaml.parent
        board_text = board_yaml.read_text(encoding="utf-8")

        for slice_ in iter_buildable_slices(project):
            if slice_.os != "zephyr":
                continue
            if not slice_.app or slice_.app == STOCK_SHIM_APP:
                continue

            app_dir = _zephyr_app_dir(slice_.app, base_dir)
            cmakelists = app_dir / "CMakeLists.txt"
            if not cmakelists.is_file():
                problems.append(
                    f"{rel_board}: core '{slice_.core_id}' app "
                    f"'{slice_.app}' resolves to {app_dir}, which has no "
                    f"CMakeLists.txt")
                continue

            core_line = _core_key_line(board_text, slice_.core_id)
            resolved_by_file.setdefault(cmakelists.resolve(), []).append(
                (slice_.core_id, board_yaml, core_line))

    for cmakelists, entries in sorted(resolved_by_file.items()):
        distinct_cores = sorted({core_id for core_id, _, _ in entries})
        if len(distinct_cores) > 1:
            rel_cmake = cmakelists.relative_to(root).as_posix()
            sources = ", ".join(
                f"{core_id} ({board_yaml.relative_to(root).as_posix()}"
                f"{f':{line}' if line else ''})"
                for core_id, board_yaml, line in entries)
            problems.append(
                f"{rel_cmake}: shared by {len(distinct_cores)} distinct "
                f"Zephyr cores -- {sources} -- each core needs its own "
                f"CMakeLists.txt (or its own per-core app dir); one shared "
                f"app dir holds ONE generated/alp.conf, so every core but "
                f"that one silently configures with the wrong Kconfig "
                f"fragment")

    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO,
                     help="repo root to scan (default: this checkout)")
    args = ap.parse_args()

    problems = find_problems(args.root)
    if problems:
        print(f"check_core_cmakelists_mapping: {len(problems)} "
              f"violation(s):", file=sys.stderr)
        for p in problems:
            print(f"  · {p}", file=sys.stderr)
        return 1
    print("check_core_cmakelists_mapping: OK -- every enabled Zephyr "
          "core's app: resolves to a CMakeLists.txt, and no two cores share one.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
