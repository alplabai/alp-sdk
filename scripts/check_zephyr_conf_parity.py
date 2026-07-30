#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Byte-parity gate: a Zephyr example's own `CMakeLists.txt` (which calls
`alp_sdk_zephyr_conf(<id>)` from `cmake/alp.cmake` at configure time) and the
planner's build-plan `configArtefacts` (`--emit build-plan`, consumed by
`tan`) MUST materialise the identical `alp.conf` for the same core.

Both paths call the same function (`alp_orchestrate.kconfig._slice_alp_conf`)
-- this gate pins that invariant byte-for-byte so a future change to either
call site (or to `_emit_library_hw_backends`, folded into `_slice_alp_conf`
so both paths share it -- see docs/adr/0020-sdk-owns-build-execution.md
addendum) can't silently fork the two paths again. Mirrors the
`check_emit_snapshots.py` byte-identical-emit precedent, scoped to this one
pinned invariant across the whole example corpus rather than a fixed golden
set, so a NEW example inherits the check automatically.

Scope: every example `CMakeLists.txt` under `examples/**` whose zephyr-conf
emit is core-scoped (single-app examples AND per-core multicore subdirs
alike) -- spelled either as `alp_sdk_zephyr_conf(<id>)` or as a raw
`alp_project.py --emit zephyr-conf --core <id>` invocation. An unscoped
(`--core`-less) raw invocation sums across cores by design and is out of
scope for a per-core byte-parity check; `_find_unscoped_emits` fails on it
rather than skipping it.

Usage:

    python3 scripts/check_zephyr_conf_parity.py
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))

from alp_orchestrate import load_board_yaml, OrchestratorError  # noqa: E402
from alp_orchestrate.kconfig import _slice_alp_conf  # noqa: E402

# The core id an example's CMakeLists.txt scopes its emit to. Two spellings
# are recognised:
#
#   * `alp_sdk_zephyr_conf(<core> [BOARD_YAML <path>])` -- the shared
#     `cmake/alp.cmake` helper every example calls today.
#   * a raw `--emit zephyr-conf --core <id>` invocation -- the pre-helper
#     shape, still matched so a hand-rolled example (or one copied from an
#     older release) stays inside this gate instead of silently dropping out
#     of the corpus.
_CORE_RE = re.compile(
    r"alp_sdk_zephyr_conf\(\s*([^\s)]+)"
    r"|--emit\s+zephyr-conf\s+--core\s+(\S+)")
# `BOARD_YAML ${CMAKE_CURRENT_SOURCE_DIR}/<rel>` (helper) or the raw
# `--input <path>` argument on any adjacent line of the same
# `execute_process(COMMAND ...)` block.
_INPUT_RE = re.compile(
    r"BOARD_YAML\s+\$\{CMAKE_CURRENT_SOURCE_DIR\}/(\S+?board\.yaml)"
    r"|COMMAND\b.*?--input\s+\$\{CMAKE_CURRENT_SOURCE_DIR\}/(\S+?board\.yaml)",
    re.DOTALL)
# Any zephyr-conf emit, `--core`-scoped or not. When a file's count of these
# exceeds its `--core`-scoped count (`_CORE_RE`), some invocation is UNSCOPED
# -- the cross-core Kconfig sum ADR-0020's addendum retired. `alp_sdk_zephyr_
# conf()` FATAL_ERRORs on an empty core argument, so the helper spelling can
# only ever be scoped; the raw spelling still can't.
_EMIT_RE = re.compile(r"alp_sdk_zephyr_conf\(|--emit\s+zephyr-conf\b")


def _code(text: str) -> str:
    """`text` with every whole-line `#` comment dropped. Both patterns below
    are matched against CODE only: `examples/**` carries 28 CMakeLists.txt
    files whose comments MENTION the emit precisely to say they do NOT invoke
    it, plus several that name `alp_sdk_zephyr_conf()` in prose -- a bare
    text match reads those as invocations and (for `_find_unscoped_emits`)
    fails the gate on a comment."""
    return "\n".join(
        line for line in text.splitlines() if not line.lstrip().startswith("#"))


def _find_cases() -> list[tuple[Path, Path, str]]:
    """(CMakeLists.txt path, board.yaml path, core id) for every scoped
    `--emit zephyr-conf --core <id>` invocation under `examples/**`."""
    cases = []
    for cmakelists in sorted(REPO.glob("examples/**/CMakeLists.txt")):
        text = _code(cmakelists.read_text(encoding="utf-8"))
        core_m = _CORE_RE.search(text)
        if core_m is None:
            continue
        input_m = _INPUT_RE.search(text)
        board_rel = "board.yaml"
        if input_m:
            board_rel = input_m.group(1) or input_m.group(2)
        board_yaml = (cmakelists.parent / board_rel).resolve()
        core_id = core_m.group(1) or core_m.group(2)
        cases.append((cmakelists, board_yaml, core_id))
    return cases


def _find_unscoped_emits(repo: Path = REPO) -> list[Path]:
    """CMakeLists.txt files with a `--emit zephyr-conf` invocation that is
    NOT `--core`-scoped -- the cross-core Kconfig leak ADR-0020's addendum
    retired. `_find_cases` silently skips a `--core`-less invocation, so
    without this guard a re-introduced unscoped emit would pass the gate
    while shipping cross-core-contaminated firmware. Fail loudly instead."""
    leaks = []
    for cmakelists in sorted(repo.glob("examples/**/CMakeLists.txt")):
        text = _code(cmakelists.read_text(encoding="utf-8"))
        if len(_EMIT_RE.findall(text)) > len(_CORE_RE.findall(text)):
            leaks.append(cmakelists)
    return leaks


def main() -> int:
    leaks = _find_unscoped_emits()
    if leaks:
        print("check_zephyr_conf_parity: unscoped `--emit zephyr-conf` "
              "(no --core) -- the cross-core Kconfig leak ADR-0020 retired; "
              "scope each to the one Zephyr core its CMakeLists.txt builds:",
              file=sys.stderr)
        for leak in leaks:
            print(f"  · {leak.relative_to(REPO).as_posix()}", file=sys.stderr)
        return 1

    cases = _find_cases()
    if not cases:
        print("check_zephyr_conf_parity: no --core-scoped zephyr-conf "
              "CMakeLists.txt found -- suspiciously empty corpus",
              file=sys.stderr)
        return 1

    failures: list[str] = []
    for cmakelists, board_yaml, core_id in cases:
        rel = cmakelists.relative_to(REPO).as_posix()
        if not board_yaml.is_file():
            failures.append(f"{rel}: board.yaml not found at {board_yaml}")
            continue
        try:
            project = load_board_yaml(board_yaml)
        except OrchestratorError as e:
            failures.append(f"{rel}: board.yaml failed to load ({e})")
            continue
        if core_id not in project.cores:
            failures.append(f"{rel}: --core {core_id} not in board.yaml")
            continue
        want = _slice_alp_conf(project, project.cores[core_id])

        proc = subprocess.run(
            [sys.executable, str(REPO / "scripts" / "alp_project.py"),
             "--input", str(board_yaml), "--emit", "zephyr-conf",
             "--core", core_id],
            capture_output=True, text=True, cwd=REPO)
        if proc.returncode != 0:
            failures.append(f"{rel}: alp_project.py --core {core_id} "
                             f"failed (rc={proc.returncode}): {proc.stderr}")
            continue
        got = proc.stdout
        if got != want:
            failures.append(
                f"{rel}: CMakeLists.txt-emitted alp.conf (core {core_id}) "
                f"!= planner-materialised alp.conf -- the two paths have "
                f"diverged")
        else:
            print(f"OK   {rel} (core {core_id})")

    if failures:
        print(f"\ncheck_zephyr_conf_parity: {len(failures)} mismatch(es):",
              file=sys.stderr)
        for f in failures:
            print(f"  · {f}", file=sys.stderr)
        return 1

    print(f"\ncheck_zephyr_conf_parity: {len(cases)} example(s), "
          f"CMakeLists.txt <-> build-plan alp.conf byte-identical.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
