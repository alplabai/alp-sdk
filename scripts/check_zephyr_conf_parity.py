#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Byte-parity gate for the per-core `alp.conf` the SDK hands to builds that do
NOT read the build plan (twister, a bare `west build`).

Those consumers get their Kconfig fragment from `scripts/gen_example_alp_conf.py`
(`<app dir>/generated/alp.conf`, named by each example's `testcase.yaml`);
`tan build` gets it from the build plan's `configArtefacts`; a human following
the docs runs `alp_project.py --emit zephyr-conf --core <id>`. All three MUST
produce the identical bytes for the same core -- this gate pins that for the
whole example corpus, so a NEW example inherits the check automatically.
Python Tan tests its relocated producer against the same contract.

All three call the same function (`alp_orchestrate.kconfig._slice_alp_conf`);
this gate pins the invariant byte-for-byte so a change to one call site (or to
`_emit_library_hw_backends`, folded into `_slice_alp_conf` -- see
docs/adr/0020-sdk-owns-build-execution.md addendum) can't silently fork them.

It also fails if an example `CMakeLists.txt` runs `alp_project.py --emit
zephyr-conf` at configure time again: that bridge was retired (#866) because it
put intermediate Python on every CMake configure, and re-adding it would hide a
missing pre-generation step rather than fix it.

Scope: every enabled Zephyr core of every `examples/**/board.yaml` whose `app:`
resolves to a customer app dir (not the stock M-core shim).

Usage:

    python3 scripts/check_zephyr_conf_parity.py
"""
from __future__ import annotations

import os
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))

from alp_orchestrate import (  # noqa: E402
    iter_buildable_slices, load_board_yaml,
)
from alp_orchestrate.kconfig import _slice_alp_conf  # noqa: E402
from alp_orchestrate.orchestrator import (  # noqa: E402
    STOCK_SHIM_APP, _zephyr_app_dir,
)

# Any `--emit zephyr-conf` in an example CMakeLists.txt is the retired
# configure-time bridge.
_EMIT_RE = re.compile(r"--emit\s+zephyr-conf\b")


def find_cases(root: Path = REPO / "examples") -> list[tuple[Path, Path, str]]:
    """(app dir, board.yaml path, core id) for every enabled Zephyr core whose
    `app:` resolves to a customer app dir under `root` (default `examples/`;
    public so gen_example_alp_conf.py and alp_template.py share it). The
    per-core `generated/alp.conf` lives in the app dir. A board.yaml that
    fails to load raises."""
    cases = []
    for board_yaml in sorted(root.glob("**/board.yaml")):
        project = load_board_yaml(board_yaml)
        for slice_ in iter_buildable_slices(project):
            if (slice_.os != "zephyr" or not slice_.app
                    or slice_.app == STOCK_SHIM_APP):
                continue
            cases.append((_zephyr_app_dir(slice_.app, board_yaml.parent),
                          board_yaml, slice_.core_id))
    return cases


def find_bridges(repo: Path = REPO) -> list[Path]:
    """CMakeLists.txt files that still run `--emit zephyr-conf` at configure
    time -- the retired bridge (#866)."""
    return [c for c in sorted(repo.glob("examples/**/CMakeLists.txt"))
            if _EMIT_RE.search(c.read_text(encoding="utf-8"))]


def main() -> int:
    failures: list[str] = []

    for cmakelists in find_bridges():
        failures.append(
            f"{cmakelists.relative_to(REPO).as_posix()}: runs `--emit "
            f"zephyr-conf` at configure time -- the bridge retired in #866; "
            f"per-core alp.conf comes from gen_example_alp_conf.py / the "
            f"build plan")

    cases = find_cases()
    if not cases:
        print("check_zephyr_conf_parity: no Zephyr example cores found "
              "-- suspiciously empty corpus", file=sys.stderr)
        return 1

    for app_dir, board_yaml, core_id in cases:
        rel = app_dir.relative_to(REPO).as_posix()
        project = load_board_yaml(board_yaml)
        want = _slice_alp_conf(project, project.cores[core_id])

        proc = subprocess.run(
            [sys.executable, str(REPO / "scripts" / "alp_project.py"),
             "--input", str(board_yaml), "--emit", "zephyr-conf",
             "--core", core_id],
            capture_output=True, text=True, encoding="utf-8",
            env={**os.environ, "PYTHONIOENCODING": "utf-8"}, cwd=REPO)
        if proc.returncode != 0:
            failures.append(f"{rel}: alp_project.py --core {core_id} "
                            f"failed (rc={proc.returncode}): {proc.stderr}")
            continue
        if proc.stdout != want:
            failures.append(
                f"{rel}: `alp_project.py --emit zephyr-conf --core "
                f"{core_id}` != planner-materialised alp.conf -- the two "
                f"paths have diverged")
        else:
            print(f"OK   {rel} (core {core_id})")

    if failures:
        print(f"\ncheck_zephyr_conf_parity: {len(failures)} problem(s):",
              file=sys.stderr)
        for f in failures:
            print(f"  · {f}", file=sys.stderr)
        return 1

    print(f"\ncheck_zephyr_conf_parity: {len(cases)} example core(s), "
          f"no CMakeLists.txt bridge, alp_project.py emit <-> "
          f"build-plan alp.conf byte-identical.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
