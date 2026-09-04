#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: no `examples/**/CMakeLists.txt` may reach `scripts/alp_project.py`
through a bare relative hop -- every invocation resolves the script under
`${ALP_SDK_ROOT}`.

Issue #1390: `examples/peripheral-io/drone-autopilot/CMakeLists.txt` invoked
the loader as
`${CMAKE_CURRENT_SOURCE_DIR}/../../../scripts/alp_project.py`. `../../..` is
the SDK root only while the example sits at `<sdk>/examples/<category>/<name>/`.
Scaffolded out of tree -- which is exactly what `tan init --from-example`
produces, and what a customer ends up with -- three levels up is somewhere
else entirely (`C:\\` for a project at `C:\\alp\\proj-drone\\aen-drone`), so the
command became `C:\\scripts\\alp_project.py` and configure died with
`can't open file ... [Errno 2] No such file or directory`. Because the hop was
unconditional, setting `ALP_SDK_ROOT` was not a workaround.

Twenty examples shared that copied block; nothing checked it, so the defect
spread by copy-paste from one example to the next. The correct shape --
`examples/peripheral-io/pwm-led-fade/CMakeLists.txt` has had it all along --
prefers the environment and falls back to the relative hop only for the
in-tree case:

    if(DEFINED ENV{ALP_SDK_ROOT})
        set(ALP_SDK_ROOT $ENV{ALP_SDK_ROOT})
    else()
        get_filename_component(ALP_SDK_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../.. ABSOLUTE)
    endif()

This gate constrains ONLY the token naming the script. The `..` inside the
`get_filename_component()` fallback is the point of that fallback and is not
flagged, and neither is a `..` anywhere else on the command (an example whose
`--input` reads `${CMAKE_CURRENT_SOURCE_DIR}/../board.yaml` -- the multicore
per-core slices do -- stays legal). The fallback's depth is deliberately not
checked: it varies correctly with nesting (`../../..` for
`examples/<cat>/<name>/`, `../../../..` for a per-core subdirectory).

Run locally:

    python3 scripts/check_example_sdk_root.py

CI wires this in pr-metadata-validate.yml.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# The token naming the script: any run of non-whitespace ending in
# `scripts/alp_project.py`.
SCRIPT_TOKEN = re.compile(r"\S*scripts/alp_project\.py")

REQUIRED_PREFIX = "${ALP_SDK_ROOT}/"


def _strip_comment(line: str) -> str:
    """Drop a trailing CMake `#` comment, ignoring `#` inside a quoted string."""
    in_quote = False
    escaped = False
    for i, ch in enumerate(line):
        if escaped:
            escaped = False
            continue
        if ch == "\\":
            escaped = True
        elif ch == '"':
            in_quote = not in_quote
        elif ch == "#" and not in_quote:
            return line[:i]
    return line


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    examples = root / "examples"
    if not examples.is_dir():
        return problems

    for cmakelists in sorted(examples.rglob("CMakeLists.txt")):
        rel = cmakelists.relative_to(root).as_posix()
        text = cmakelists.read_text(encoding="utf-8")
        for lineno, raw in enumerate(text.splitlines(), 1):
            # Prose mentioning the loader ("Plain prj.conf app (no
            # alp_project.py board.yaml emit)") is not an invocation.
            code = _strip_comment(raw)
            for token in SCRIPT_TOKEN.findall(code):
                if token.startswith(REQUIRED_PREFIX):
                    continue
                problems.append(
                    f"{rel}:{lineno}: invokes alp_project.py as {token!r} -- "
                    f"resolve it as '{REQUIRED_PREFIX}scripts/alp_project.py' "
                    f"and set ALP_SDK_ROOT from the environment with an "
                    f"in-tree get_filename_component() fallback, as "
                    f"examples/peripheral-io/pwm-led-fade/CMakeLists.txt does. "
                    f"A bare relative hop resolves nowhere once the example is "
                    f"scaffolded out of tree (issue #1390)"
                )

    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    problems = find_problems(args.root)
    if problems:
        print("check_example_sdk_root: found problems:", file=sys.stderr)
        for p in problems:
            print(f"  {p}", file=sys.stderr)
        return 1
    print("OK: every example CMakeLists.txt resolves alp_project.py "
          "under ${ALP_SDK_ROOT}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
