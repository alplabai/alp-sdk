#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Gate: example READMEs must teach the `tan build` flow and name the right board.

Why (2026-10 audit, #2885): 111 of 165 example READMEs carried only raw
`west build` recipes, which do not work outside an SDK checkout; 16 named
`ensemble_e8_dk`, a board CI never builds.  A README that cannot be followed
is worse than none.

For every examples/**/README.md whose directory also holds a board.yaml:

  R1  the README needs a fenced code block with a line starting `tan build`.
  R2  README.md / testcase.yaml must not mention `ensemble_e8_dk` while
      board.yaml som.sku starts with "E1M-".
  R3  every fenced `west build ... -b <target>` must match board.yaml: the
      target starts with alp_e1m_<sku lowercased, 'E1M-' and '-' removed>_
      and its core segment (m55_hp / m55_he / m33_sm) must be a core declared
      in board.yaml `cores` with os != "off".  A target the sibling
      testcase.yaml builds (platform_allow / integration_platforms) is
      accepted as-is: that covers the AEN801/AEN803 twin and HE/HP twin
      builds CI already runs.  `-b native_sim...` is ignored, and so is a
      fenced block preceded by `<!-- example-readme:other-sku -->`: a
      recipe the README documents for a board.yaml edited to another SKU
      (same-PCB V2N/V2M, or "for V2N silicon, change som.sku first").

Output: one line per violation, `<path>:<line>: <rule> <message>`; exit 1 on
any violation, 0 otherwise.  Fix by editing the README (lead with a
`tan build` block; use the example's real SoM/core target).
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

import yaml

CORES = ("m55_hp", "m55_he", "m33_sm")
FENCE_RE = re.compile(r"^\s*(```|~~~)")
BAD_BOARD = "ensemble_e8_dk"
OTHER_SKU_MARK = "<!-- example-readme:other-sku -->"
BUILD_B_RE = re.compile(r"(?:^|\s)-b\s*=?\s*(\S+)")


def _load_yaml(path: Path) -> dict:
    try:
        data = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, yaml.YAMLError):
        return {}
    return data if isinstance(data, dict) else {}


def _tan_build_in_fence(lines: list[str]) -> bool:
    in_fence = False
    for line in lines:
        if FENCE_RE.match(line):
            in_fence = not in_fence
        elif in_fence and line.lstrip().startswith("tan build"):
            return True
    return False


def _west_build_commands(lines: list[str]) -> list[tuple[int, str]]:
    """Fenced `west build` commands as (1-based start line, text after
    `west build`), backslash continuations joined.  Prose and inline code are
    skipped: a sentence warning against a raw build is not a recipe."""
    cmds: list[tuple[int, str]] = []
    in_fence = skip_block = False
    i = 0
    while i < len(lines):
        if FENCE_RE.match(lines[i]):
            in_fence = not in_fence
            prev = next((ln.strip() for ln in reversed(lines[:i]) if ln.strip()), "")
            skip_block = in_fence and prev == OTHER_SKU_MARK
        elif in_fence and not skip_block and "west build" in lines[i]:
            start, text = i + 1, lines[i]
            while text.rstrip().endswith("\\") and i + 1 < len(lines):
                i += 1
                text = text.rstrip()[:-1] + " " + lines[i]
            cmds.append((start, text.split("west build", 1)[1]))
        i += 1
    return cmds


def _testcase_targets(path: Path) -> set[str]:
    """Every platform the sibling testcase.yaml names for a scenario."""
    tests = _load_yaml(path).get("tests") or {}
    found: set[str] = set()
    for scenario in tests.values():
        if not isinstance(scenario, dict):
            continue
        for key in ("platform_allow", "integration_platforms"):
            value = scenario.get(key) or []
            found.update([value] if isinstance(value, str) else value)
    return found


def _find_line(lines: list[str], needle: str) -> int:
    for n, line in enumerate(lines, 1):
        if needle in line:
            return n
    return 1


def check_example(readme: Path, root: Path) -> list[str]:
    board = _load_yaml(readme.parent / "board.yaml")
    rel = readme.relative_to(root).as_posix()
    lines = readme.read_text(encoding="utf-8").splitlines()
    sku = str((board.get("som") or {}).get("sku") or "")
    out: list[str] = []

    if not _tan_build_in_fence(lines):
        out.append(f"{rel}:1: R1 no fenced code block with a `tan build` line")

    if sku.startswith("E1M-"):
        if BAD_BOARD in "\n".join(lines):
            out.append(f"{rel}:{_find_line(lines, BAD_BOARD)}: R2 mentions "
                       f"{BAD_BOARD} but board.yaml som.sku is {sku}")
        tc = readme.parent / "testcase.yaml"
        if tc.is_file():
            tc_lines = tc.read_text(encoding="utf-8").splitlines()
            if any(BAD_BOARD in ln for ln in tc_lines):
                trel = tc.relative_to(root).as_posix()
                out.append(f"{trel}:{_find_line(tc_lines, BAD_BOARD)}: R2 "
                           f"mentions {BAD_BOARD} but board.yaml som.sku is {sku}")

    prefix = "alp_e1m_" + sku.lower().removeprefix("e1m-").replace("-", "") + "_"
    declared = {
        name for name, cfg in (board.get("cores") or {}).items()
        if isinstance(cfg, dict) and cfg.get("os") != "off"
    }
    built_by_ci = _testcase_targets(readme.parent / "testcase.yaml")
    for lineno, cmd in _west_build_commands(lines):
        m = BUILD_B_RE.search(cmd)
        if not m:
            continue
        target = m.group(1).strip("\"'`")
        if target.startswith("native_sim") or not sku.startswith("E1M-"):
            continue
        if target in built_by_ci:
            continue
        if not target.startswith(prefix):
            out.append(f"{rel}:{lineno}: R3 target '{target}' does not start "
                       f"with '{prefix}' (board.yaml som.sku {sku})")
            continue
        core = next((c for c in CORES if target[len(prefix):].startswith(c)), None)
        if core and core not in declared:
            out.append(f"{rel}:{lineno}: R3 core '{core}' in '{target}' is not "
                       f"an enabled core in board.yaml (enabled: "
                       f"{', '.join(sorted(declared)) or 'none'})")
    return out


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    for readme in sorted((root / "examples").rglob("README.md")):
        if (readme.parent / "board.yaml").is_file():
            problems.extend(check_example(readme, root))
    return problems


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description="Check example READMEs against board.yaml (rules R1-R3).")
    ap.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1],
                    help="repo root (default: this checkout)")
    args = ap.parse_args(argv)
    problems = find_problems(args.root)
    for p in problems:
        print(p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
