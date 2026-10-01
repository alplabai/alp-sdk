#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Pre-generate each Zephyr example's per-core ``generated/alp.conf``.

Twister and a bare ``west build`` configure an example straight from its
source dir with no SDK-side step in front of them, so the per-core Kconfig
fragment (`alp_orchestrate.kconfig._slice_alp_conf`, the same function the
build plan's ``configArtefacts`` use) has to exist on disk BEFORE they run.
Each example's ``testcase.yaml`` points ``EXTRA_CONF_FILE`` at
``generated/alp.conf``; it lives under ``generated/`` (git-ignored) because
Zephyr auto-merges every ``*.conf`` beside ``prj.conf``.

Walks every example ``CMakeLists.txt`` with a ``--core``-scoped
``--emit zephyr-conf`` (the corpus ``check_zephyr_conf_parity.py`` pins) and
writes ``<example dir>/generated/alp.conf`` for its core.

Usage:

    python3 scripts/gen_example_alp_conf.py [example_dir ...]

With no arguments every example is generated.
"""
from __future__ import annotations

import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))

import check_zephyr_conf_parity as parity  # noqa: E402
from alp_orchestrate import load_board_yaml, OrchestratorError  # noqa: E402
from alp_orchestrate.kconfig import _slice_alp_conf  # noqa: E402


def generate(cmakelists: Path, board_yaml: Path, core_id: str) -> Path:
    """Write and return ``<cmakelists dir>/generated/alp.conf``."""
    project = load_board_yaml(board_yaml)
    if core_id not in project.cores:
        raise OrchestratorError(f"--core {core_id} not in {board_yaml}")
    out = cmakelists.parent / "generated" / "alp.conf"
    out.parent.mkdir(parents=True, exist_ok=True)
    # newline="" keeps the fragment byte-identical to the planner's on Windows.
    with open(out, "w", encoding="utf-8", newline="") as f:
        f.write(_slice_alp_conf(project, project.cores[core_id]))
    return out


def main(argv: list[str]) -> int:
    only = {Path(a).resolve() for a in argv}
    failures, n = [], 0
    seen: set[Path] = set()
    for cmakelists, board_yaml, core_id in parity.find_cases():
        if only and cmakelists.parent.resolve() not in only:
            continue
        seen.add(cmakelists.parent.resolve())
        rel = cmakelists.relative_to(REPO).as_posix()
        if rel in parity.EXCLUDED_WITH_REASON:
            print(f"SKIP {rel}: {parity.EXCLUDED_WITH_REASON[rel]}")
            continue
        try:
            generate(cmakelists, board_yaml, core_id)
            n += 1
        except (OrchestratorError, OSError) as e:
            failures.append(f"{rel}: {e}")
    for d in sorted(only - seen):
        failures.append(f"{d}: matches no --core zephyr-conf example")
    for f in failures:
        print(f"gen_example_alp_conf: {f}", file=sys.stderr)
    print(f"gen_example_alp_conf: wrote {n} generated/alp.conf")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
