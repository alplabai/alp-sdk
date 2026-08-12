#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Core-to-CMakeLists mapping gate: fails (exit 1) when an example's baked
`--emit zephyr-conf --core <id>` literal disagrees with the board.yaml core
whose `app:` resolves to it, or when two distinct Zephyr cores resolve to the
same CMakeLists.txt.

On a dual-Zephyr-core SKU (e.g. E1M-AEN801, M55-HE + M55-HP), adding a
second `cores.<id>.app:` that resolves to an ALREADY-WIRED sibling core's
app directory leaves that sibling's baked-in
`alp_project.py --emit zephyr-conf --core <id>` literal in place: the new
core's `west build` configures with the OTHER core's Kconfig fragment while
the orchestrator separately materialises the correct per-core `alp.conf` --
two Kconfig fragments for two different cores in one build, silently, with
no warning.

Two invariants, BOTH required:

  1. Literal agreement -- resolving `app:` for every enabled Zephyr core
     (via `alp_orchestrate.orchestrator._zephyr_app_dir`, the exact function
     that decides `west build`'s app-dir argument -- see that module's
     docstring for the two supported conventions) must land on a
     CMakeLists.txt whose baked `--emit zephyr-conf --core <id>` literal is
     THAT SAME core's id.
  2. No sharing -- two DISTINCT cores must never resolve `app:` to the same
     CMakeLists.txt.

Both assertions are load-bearing, but be precise about when each one is the
ONLY thing standing between you and the defect -- mutation-tested, not assumed:

  * In the common shape of the trap (a second core added onto a sibling's
    `./src`, that sibling's CMakeLists baking `--core <sibling>`), assertion 1
    ALSO fires -- it catches the newly-added core, whose resolved literal is
    the sibling's, not its own. Assertion 2 is not independently required here.
  * Assertion 2 is independently required when the shared CMakeLists.txt bakes
    NO `--core` literal at all: assertion 1 then has nothing to compare and
    structurally cannot fire, while assertion 2 still reports the sharing.
    No such file exists in the corpus today (all checked CMakeLists carry a
    literal), so this is the case assertion 2 exists to keep catching as the
    corpus changes.

The SDK-owned stock M-core shim (`alp-stock-shim`, firmware/alp-stock-shim/)
is EXCLUDED from both checks: every core a board.yaml leaves at its SoM
topology default resolves there BY DESIGN (metadata/e1m_modules/*.yaml
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
from alp_orchestrate.sdk_compat import (  # noqa: E402
    assert_exclusion_still_not_buildable,
)

# Same shape as check_zephyr_conf_parity.py's `_CORE_RE`: the baked-in
# `--core <id>` a Zephyr example's CMakeLists.txt passes to
# `alp_project.py --emit zephyr-conf`. Defined independently here (not
# imported) -- this gate's invariant (does the literal match the core
# whose app: resolves to this file) is distinct from that gate's
# (does the materialised alp.conf byte-match), so the two scripts stay
# free to diverge on match tolerance without coupling to each other.
_CORE_RE = re.compile(r"--emit\s+zephyr-conf\s+--core\s+(\S+)")

# board.yaml paths (repo-relative) that cannot load AT ALL right now, with
# the reason -- mirrors check_zephyr_conf_parity.py's
# `_EXCLUDED_WITH_REASON` (same #1025 root cause: a board.yaml that can't
# even load has nothing for this gate to walk, which is a different, honest
# failure this gate isn't the one to report). RATCHET: `find_problems` re-
# asserts each entry's (family_dir, hw_rev) every run via
# `assert_exclusion_still_not_buildable` and fails loudly the moment the
# reason stops holding -- this dict alone is not self-enforcing.
_EXCLUDED_WITH_REASON: dict[str, str] = {
    "examples/multicore/rpmsg-imx93/board.yaml":
        "E1M-NX9101's only hw_rev (imx93 r1) is `status: tbd` -- refused "
        "outright by the hw_rev-buildable gate (#1025). Remove this entry "
        "once metadata/e1m_modules/imx93/hw-revisions.yaml:r1 carries a "
        "buildable status.",
}
# (family_dir, hw_rev) each excluded board.yaml's reason cites, for the
# ratchet assertion above -- keyed the same as _EXCLUDED_WITH_REASON.
_EXCLUDED_FAMILY_HWREV: dict[str, tuple[str, str]] = {
    "examples/multicore/rpmsg-imx93/board.yaml": ("imx93", "r1"),
}


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
        if rel_board in _EXCLUDED_WITH_REASON:
            family_dir, hw_rev = _EXCLUDED_FAMILY_HWREV[rel_board]
            stale = assert_exclusion_still_not_buildable(
                root / "metadata", family_dir, hw_rev,
                gate=f"check_core_cmakelists_mapping.py ({rel_board})")
            if stale:
                problems.append(stale)
            continue
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

            text = cmakelists.read_text(encoding="utf-8")
            m = _CORE_RE.search(text)
            if m is None:
                continue  # nothing baked to compare (assertion 2 still runs)
            literal = m.group(1)
            if literal != slice_.core_id:
                rel_cmake = cmakelists.relative_to(root).as_posix()
                line = _line_of(text, m.start())
                problems.append(
                    f"{rel_cmake}:{line}: baked `--core {literal}` but "
                    f"board.yaml core '{slice_.core_id}' "
                    f"({rel_board}"
                    f"{f':{core_line}' if core_line else ''}) resolves "
                    f"app: '{slice_.app}' here -- literal agreement "
                    f"violated")

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
                f"file can only carry ONE core's baked `--core` literal, so "
                f"every core but that one silently configures with the "
                f"wrong Kconfig fragment")

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
          "core's app: resolves to a CMakeLists.txt baked with that core's "
          "own --core literal, and no two cores share one.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
