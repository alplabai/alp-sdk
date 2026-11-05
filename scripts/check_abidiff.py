#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""
Binary ABI gate: `abidiff` (libabigail) on the built libalp_sdk.so (#2840).

Since #2839 the soname carries the minor while the major is 0
(libalp_sdk.so.0.17), so a patch release inside one minor line must stay
ABI-compatible -- that is what makes an in-place package upgrade on the
dev-kit feed safe (#2827).  `pr-abi-snapshot` only diffs the HEADERS, and
only on main/release/**; this gate diffs the compiled library.

What it does:
  1. Picks the baseline: the highest release tag vX.Y.Z of the same
     MAJOR.MINOR as metadata/sdk_version.yaml, else that minor's highest
     rc tag, else SKIPS ("first release of minor X.Y, nothing to compare").
  2. Builds libalp_sdk.so from the baseline tag and from the working tree
     with the same host compiler and the same options (the yocto
     plain-CMake shared configuration, RelWithDebInfo so DWARF is present).
  3. `abidiff --no-added-syms` with --headers-dir1/2 = each tree's
     include/ (public surface only).  Added symbols are compatible and
     are not reported.
  4. Fails when abidiff reports ABI_CHANGE (4) or ABI_INCOMPATIBLE_CHANGE
     (8) and the baseline is the same minor as the head.  Against a
     different-minor baseline (--baseline-tag; the SONAME differs) the
     diff is reported and the gate passes.

[ABI-EXPERIMENTAL] is deliberately NOT suppressed: it is a header-comment
tag that no tool extracts per symbol, and its "may change in any minor"
latitude is a minor-to-minor allowance; this gate only compares within a
minor, where nothing may break.

The gate is ADVISORY: it is not a required context and has no merge_group
trigger (maintainer decision, #2851).  Every skip emits a ::warning:: under
--require-tool or in CI (GITHUB_ACTIONS) so it is visible on the run page.

A missing abidiff SKIPS loudly (exit 0, banner on stderr) unless
--require-tool is given (CI), in which case it fails.

Usage:
    python3 scripts/check_abidiff.py [--require-tool] [--baseline-tag TAG]
"""
from __future__ import annotations

import argparse
import io
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

ABI_ERROR, ABI_USAGE, ABI_CHANGE, ABI_INCOMPATIBLE = 1, 2, 4, 8

BUILD_ARGS = [
    "-DALP_OS=yocto",
    "-DALP_SDK_BUILD_SHARED=ON",
    "-DCMAKE_BUILD_TYPE=RelWithDebInfo",
]


def warn_skip(msg: str, always: bool = False) -> None:
    """GitHub ::warning:: for a skipped check (CI or --require-tool)."""
    if always or os.environ.get("GITHUB_ACTIONS"):
        print(f"::warning title=abidiff skipped::{msg}")


def sdk_minor(root: Path) -> str:
    """'0.17' from the `version:` line of metadata/sdk_version.yaml."""
    text = (root / "metadata" / "sdk_version.yaml").read_text(encoding="utf-8")
    m = re.search(r"^version:\s*(\d+)\.(\d+)\.\d+", text, re.M)
    if not m:
        raise ValueError("metadata/sdk_version.yaml: no parsable `version:`")
    return f"{m[1]}.{m[2]}"


def pick_baseline(minor: str, tags: list[str]) -> str | None:
    """Highest release tag of `minor`, else its highest rc tag, else None."""
    pre = re.escape(minor)
    rel = [(int(m[1]),) for t in tags if (m := re.fullmatch(rf"v{pre}\.(\d+)", t))]
    if rel:
        return f"v{minor}.{max(rel)[0]}"
    rcs = [
        (int(m[1]), int(m[2]), t)
        for t in tags
        if (m := re.fullmatch(rf"v{pre}\.(\d+)-rc(\d+)", t))
    ]
    return max(rcs)[2] if rcs else None


def evaluate(rc: int, same_minor: bool) -> tuple[bool, str]:
    """(failed, reason) for an abidiff exit status."""
    if rc & (ABI_ERROR | ABI_USAGE):
        return True, f"abidiff itself failed (exit {rc})"
    changed = bool(rc & (ABI_CHANGE | ABI_INCOMPATIBLE))
    if not changed:
        return False, "no ABI change"
    if not same_minor:
        return False, "ABI changed but SONAME differs (new minor): report only"
    return True, (
        "incompatible ABI change inside one minor line: a patch release must "
        "stay ABI-compatible (in-place package upgrades depend on it). Revert "
        "the change, or make it in a new minor (which bumps the SONAME)"
    )


def soname_only(report: str) -> bool:
    """True when abidiff's only finding is the ELF SONAME rename.

    A baseline built before #2839 carries SONAME libalp_sdk.so.0; comparing
    it to a same-minor head always sets ABI_CHANGE for that rename alone.
    Every "changes summary" line being all-zero means nothing else moved.
    """
    lines = re.findall(r"changes summary: (.*)", report)
    nums = [n for ln in lines for n in re.findall(r"(\d+) (?:Removed|Changed|Added)", ln)]
    return len(lines) >= 2 and all(n == "0" for n in nums) and "SONAME changed" in report


def soname_of(lib: Path) -> str | None:
    if not shutil.which("readelf"):
        return None
    out = subprocess.run(
        ["readelf", "-d", str(lib)], capture_output=True, text=True, encoding="utf-8", check=False
    ).stdout
    m = re.search(r"SONAME.*\[(.+)\]", out)
    return m[1] if m else None


def build_lib(src: Path, build: Path) -> Path:
    cfg = ["cmake", "-S", str(src), "-B", str(build), *BUILD_ARGS]
    for gen, tool in (("Ninja", "ninja"), ("Unix Makefiles", "make")):
        if shutil.which(tool):
            cfg += ["-G", gen]
            break
    subprocess.run(cfg, check=True)
    subprocess.run(["cmake", "--build", str(build), "--parallel"], check=True)
    libs = sorted(build.rglob("libalp_sdk.so"))
    if not libs:
        raise FileNotFoundError(f"no libalp_sdk.so under {build}")
    return libs[0]


def export_tag(root: Path, tag: str, dest: Path) -> None:
    tar = subprocess.run(
        ["git", "-C", str(root), "archive", "--format=tar", tag],
        capture_output=True, check=True,
    ).stdout
    with tarfile.open(fileobj=io.BytesIO(tar)) as tf:
        tf.extractall(dest)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--root", type=Path, default=REPO)
    ap.add_argument("--baseline-tag", help="override the auto-picked baseline")
    ap.add_argument("--require-tool", action="store_true",
                    help="missing abidiff/cmake fails instead of skipping (CI)")
    ap.add_argument("--keep", action="store_true", help="keep the temp build dirs")
    args = ap.parse_args()

    for tool in ("abidiff", "cmake"):
        if not shutil.which(tool):
            msg = f"{tool} not found"
            if args.require_tool:
                warn_skip(f"{msg}; nothing was checked", True)
                print(f"FAIL: {msg} (--require-tool)", file=sys.stderr)
                return 1
            warn_skip(f"{msg}; nothing was checked")
            print("=" * 70, file=sys.stderr)
            print(f"SKIPPED check_abidiff: {msg}. NOTHING WAS CHECKED.", file=sys.stderr)
            print("Install libabigail (apt install abigail-tools) to run this gate.",
                  file=sys.stderr)
            print("=" * 70, file=sys.stderr)
            return 0

    root = args.root
    minor = sdk_minor(root)
    tags = subprocess.run(["git", "-C", str(root), "tag", "--list", f"v{minor}.*"],
                          capture_output=True, text=True, encoding="utf-8", check=True).stdout.split()
    tag = args.baseline_tag or pick_baseline(minor, tags)
    if tag is None:
        warn_skip(f"no v{minor}.* tag: nothing compared until the first tag of "
                  f"minor {minor}", args.require_tool)
        print(f"SKIP: first release of minor {minor}, nothing to compare "
              f"(no v{minor}.* tag; in CI make sure tags are fetched).")
        return 0

    work = Path(tempfile.mkdtemp(prefix="abidiff-"))
    try:
        base_src = work / "base-src"
        base_src.mkdir()
        export_tag(root, tag, base_src)
        base_lib = build_lib(base_src, work / "base-build")
        head_lib = build_lib(root, work / "head-build")
        s1, s2 = soname_of(base_lib), soname_of(head_lib)
        print(f"baseline {tag}: SONAME {s1}; head: SONAME {s2}")
        proc = subprocess.run(
            ["abidiff", "--no-added-syms",
             "--headers-dir1", str(base_src / "include"),
             "--headers-dir2", str(root / "include"),
             str(base_lib), str(head_lib)],
            capture_output=True, text=True, encoding="utf-8", check=False,
        )
        print(proc.stdout, end="")
        print(proc.stderr, end="", file=sys.stderr)
        same_minor = tag.startswith(f"v{minor}.")
        rc = proc.returncode
        if not (rc & (ABI_ERROR | ABI_USAGE)) and same_minor and s1 != s2 and soname_only(proc.stdout):
            print("note: only the SONAME rename differs (pre-#2839 baseline); ignored")
            rc = 0
        # same_minor is false only with a --baseline-tag override.
        failed, why = evaluate(rc, same_minor)
        print(("FAIL: " if failed else "OK: ") + why)
        return 1 if failed else 0
    finally:
        if args.keep:
            print(f"kept {work}")
        else:
            shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
