#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
Reject examples that claim Alp storage coverage while using Zephyr storage APIs.

This lint is intentionally narrow.  Low-level Zephyr disk/flash bring-up demos
are allowed, but their testcase metadata must not describe them as validating
`<alp/storage.h>` or the portable Alp storage surface.
"""

from __future__ import annotations

import argparse
import pathlib
import re
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parent.parent

_ALP_STORAGE_CLAIM_RE = re.compile(
    r"(?:<alp/storage\.h>|alp[- ]storage|portable\s+storage|storage\s+surface)",
    re.IGNORECASE,
)
_ZEPHYR_STORAGE_API_RE = re.compile(
    r"(?:<zephyr/(?:storage/disk_access|drivers/flash)\.h>|"
    r"\b(?:disk_access_[a-z0-9_]+|flash_(?:read|write|erase))\b)"
)
_SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".h", ".hh", ".hpp"}
_SKIP_DIRS = {"build", ".git", ".west", "__pycache__"}


def _git_root(path: pathlib.Path) -> pathlib.Path | None:
    try:
        proc = subprocess.run(
            ["git", "-C", str(path), "rev-parse", "--show-toplevel"],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return pathlib.Path(proc.stdout.strip()).resolve()


def _git_ls_files(root: pathlib.Path, pathspec: str) -> list[pathlib.Path] | None:
    try:
        proc = subprocess.run(
            ["git", "-C", str(root), "ls-files", pathspec],
            check=True,
            capture_output=True,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    return [root / line for line in proc.stdout.splitlines() if line]


def example_dirs(root: pathlib.Path) -> list[pathlib.Path]:
    tracked = _git_ls_files(root, "examples/**/testcase.yaml")
    if tracked is not None:
        return sorted({p.parent for p in tracked})

    examples_root = root / "examples"
    out: list[pathlib.Path] = []
    for d in examples_root.iterdir():
        if not d.is_dir():
            continue
        if (d / "testcase.yaml").exists():
            out.append(d)
        else:
            out.extend(sub for sub in d.iterdir()
                       if sub.is_dir() and (sub / "testcase.yaml").exists())
    return sorted(out)


def source_files(example_dir: pathlib.Path) -> list[pathlib.Path]:
    git_root = _git_root(example_dir)
    if git_root is not None:
        rel_dir = example_dir.resolve().relative_to(git_root).as_posix()
        tracked = _git_ls_files(git_root, f"{rel_dir}/**")
        if tracked is not None:
            return sorted(p for p in tracked
                          if p.suffix in _SOURCE_SUFFIXES)

    out: list[pathlib.Path] = []
    for p in example_dir.rglob("*"):
        if not p.is_file() or p.suffix not in _SOURCE_SUFFIXES:
            continue
        rel_parts = p.relative_to(example_dir).parts
        if any(part in _SKIP_DIRS for part in rel_parts[:-1]):
            continue
        out.append(p)
    return sorted(out)


def claims_alp_storage(testcase: pathlib.Path) -> bool:
    text = testcase.read_text(encoding="utf-8", errors="replace")
    return _ALP_STORAGE_CLAIM_RE.search(text) is not None


def zephyr_storage_hits(example_dir: pathlib.Path) -> list[tuple[str, int, str]]:
    hits: list[tuple[str, int, str]] = []
    for src in source_files(example_dir):
        rel = src.relative_to(example_dir).as_posix()
        text = src.read_text(encoding="utf-8", errors="replace")
        for line_no, line in enumerate(text.splitlines(), start=1):
            match = _ZEPHYR_STORAGE_API_RE.search(line)
            if match:
                hits.append((rel, line_no, match.group(0)))
    return hits


def check_example(example_dir: pathlib.Path) -> list[str]:
    testcase = example_dir / "testcase.yaml"
    if not testcase.exists() or not claims_alp_storage(testcase):
        return []
    return [
        f"{example_dir.name}: testcase.yaml claims Alp storage coverage, "
        f"but {rel}:{line_no} uses `{match}`"
        for rel, line_no, match in zephyr_storage_hits(example_dir)
    ]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=pathlib.Path, default=ROOT,
                        help="Repository root (default: this checkout)")
    args = parser.parse_args(argv)

    root = args.root.resolve()
    errors: list[str] = []
    for ex in example_dirs(root):
        errors.extend(check_example(ex))

    if errors:
        for err in errors:
            print(f"FAIL  {err}", file=sys.stderr)
        print(f"example-storage-claims: {len(errors)} issue(s) -- failing.",
              file=sys.stderr)
        return 1

    print("example-storage-claims: OK (no false Alp storage claims).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
