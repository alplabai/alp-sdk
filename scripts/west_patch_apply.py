#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""Run `west patch apply` and REFUSE a no-op (issue #1392, item 3).

`west patch apply` exits 0 in two distinct "did nothing" cases, both read out
of the pinned Zephyr v4.4.1 `scripts/west_commands/patch.py`:

  * **Nothing to read.** `load_yml()` prints
    `no patches to apply: {args.patch_yml} not found` and returns None, and
    `do_run()` then returns -- exit 0. This is what `-b`/`-l` resolving
    against the manifest repo produced in workspace `C:\\alp\\ws1380`:
    `west patch -b zephyr/patches -l zephyr/patches.yml apply` looked for
    `...\\alp-sdk\\alp-sdk\\zephyr\\patches.yml`, did not find it, and
    reported success. The same run without the flags applied **6 patches**.

  * **Nothing resolvable.** `apply()` does
    `mod = self.get_module_path(patch_info["module"]); if mod is None:
    continue` -- an unresolvable `module:` is skipped with no warning and is
    simply missing from the final
    `{patch_count} patches applied successfully \\o/` line. That is the
    already-recorded #1076 defect (`module: hal_alif` never matched
    hal_alif's own `zephyr/module.yml` name, `alif`), reached through a
    different door.

Either way the build then proceeds against an unpatched workspace. For AEN
that is not cosmetic: `zephyr/patches/mcuboot/0001-flash_map_extended-alif-
mram-base.patch` is what teaches `flash_device_base()` about
`alif_mram_flash_controller`; without it MCUboot computes the chainload
vector table as `0x0 + 0x10000 + 0x800` -- unused ITCM -- and cannot
chainload the application from MRAM. Every layer still reports success; the
first symptom is a board that does not boot.

So this wrapper does what the bare command cannot: it counts. It resolves
`zephyr/patches.yml` itself and fails BEFORE invoking west if that path does
not exist, then requires west to report that it applied exactly as many
patches as the file carries.

Usage
-----

    python3 scripts/west_patch_apply.py                     # CI / bench
    python3 scripts/west_patch_apply.py --expect-applied 2  # filtered run
    python3 scripts/west_patch_apply.py -- -dm zephyr       # west passthrough

Extra arguments after `--` are inserted before west's own `apply` subcommand
(`west patch -l ... -b ... -dm zephyr apply`), because `-dm/--dst-module`
(like `-sm`/`-w`) is a parent-parser option of `west patch`, not of `apply` --
placed after `apply` as zephyr v4.4.1's argparse rejects it (rc=2,
`unexpected arguments`). This means the passthrough only reaches `west
patch`'s own options; `apply`'s single subcommand-local option,
`-r`/`--roll-back`, is NOT reachable through it.

Deliberately NOT a `scripts/check_*.py`: this is not a static repo-invariant
gate (it mutates the workspace), so it is not a `metadata/quality-tasks-v1.
json` entry. Verifying that the patched symbols are actually present in a
workspace is a separate check -- see issue #1392 item 1.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

import yaml

ROOT = Path(__file__).resolve().parent.parent

# `west patch apply`'s success line, verbatim from patch.py:
#     self.inf(f"{patch_count} patches applied successfully \\o/")
# Deliberately unanchored: west routes this through its own logger, so a
# prefix (colorama escape, `--verbose` decoration) must not turn a real
# 6-of-6 apply into a false "applied nothing" refusal.
APPLIED_RE = re.compile(r"(\d+)\s+patches applied successfully")


class PatchApplyError(Exception):
    """A refusal: something about the apply did not add up."""


def expected_patch_count(patches_yml: Path) -> int:
    """Number of entries `patches_yml` carries.

    Raises PatchApplyError if the file is missing, unreadable, malformed, or
    carries no entries -- a missing file is the exact `-b`/`-l` no-op this
    wrapper exists to refuse, so it is an error here rather than a 0.
    """
    if not patches_yml.is_file():
        raise PatchApplyError(
            f"{patches_yml} not found -- `west patch apply` would print "
            f"'no patches to apply: <path> not found' and exit 0 having "
            f"applied nothing (#1392)"
        )
    try:
        data = yaml.safe_load(patches_yml.read_text(encoding="utf-8"))
    except yaml.YAMLError as exc:
        raise PatchApplyError(f"malformed yaml {patches_yml}: {exc}") from exc
    patches = (data or {}).get("patches") or []
    if not isinstance(patches, list):
        raise PatchApplyError(f"{patches_yml}: `patches:` is not a list")
    if not patches:
        raise PatchApplyError(
            f"{patches_yml} carries no patches -- `west patch apply` returns "
            f"immediately and exits 0 (#1392)"
        )
    return len(patches)


def parse_applied_count(output: str) -> int | None:
    """The N from west's `N patches applied successfully \\o/`, or None.

    None means west never printed the line at all -- the `load_yml()`
    early-return path, which produces `no patches to apply: ... not found`
    and exit 0.
    """
    match = APPLIED_RE.search(output)
    return int(match.group(1)) if match else None


def run_west_patch_apply(
    cwd: Path, patches_yml: Path, extra_args: list[str]
) -> tuple[int, str]:
    """Run `west patch apply` in `cwd`, returning (returncode, output).

    `-l patches_yml` and `-b <patches_yml's sibling `patches/` dir>` are
    passed explicitly so west opens the exact file `expected_patch_count()`
    just counted -- without them west falls back to its own default
    resolution of `zephyr/patches.yml` against `manifest.path`, which for a
    caller-supplied `--patches-yml` can silently be a *different* file than
    the one this wrapper counted (issue #1392): the "N applied" the wrapper
    reports would then describe a manifest west never opened. `cwd` is the
    manifest repo; stderr is folded into stdout so the CI log keeps west's
    own ordering.

    `extra_args` is inserted BEFORE `apply`, not after: `-dm`/`-sm`/`-w` are
    `west patch`'s own parent-parser options (zephyr v4.4.1
    scripts/west_commands/patch.py), and west's argparse rejects them placed
    after the `apply` subcommand (rc=2). Only `apply`'s own `-r/--roll-back`
    needs the opposite placement, so it is not reachable through this list.
    """
    proc = subprocess.run(
        [
            "west",
            "patch",
            "-l",
            str(patches_yml),
            "-b",
            str(patches_yml.parent / "patches"),
            *extra_args,
            "apply",
        ],
        cwd=str(cwd),
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )
    return proc.returncode, proc.stdout


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--patches-yml",
        type=Path,
        default=ROOT / "zephyr" / "patches.yml",
        help="patches.yml to apply (default: <sdk>/zephyr/patches.yml)",
    )
    ap.add_argument(
        "--expect-applied",
        type=int,
        default=None,
        metavar="N",
        help="require exactly N applied patches instead of the count "
             "zephyr/patches.yml carries (for a `-dm`-filtered run)",
    )
    ap.add_argument(
        "west_args",
        nargs=argparse.REMAINDER,
        help="extra `west patch` parent-parser options (e.g. `-dm MODULE`), "
             "passed after a `--` separator and inserted before the `apply` "
             "subcommand -- NOT for apply-only options such as "
             "`-r/--roll-back`, which this wrapper cannot reach",
    )
    args = ap.parse_args(argv)

    patches_yml = args.patches_yml.resolve()
    try:
        carried = expected_patch_count(patches_yml)
    except PatchApplyError as exc:
        print(f"west-patch-apply: {exc}", file=sys.stderr)
        return 1

    expected = args.expect_applied if args.expect_applied is not None else carried
    extra = [a for a in args.west_args if a != "--"]

    # cwd is the manifest repo; -l/-b (below) are what actually pin west to
    # this exact patches_yml rather than west's own default resolution.
    cwd = patches_yml.parent.parent
    rc, output = run_west_patch_apply(cwd, patches_yml, extra)
    if output:
        print(output, end="" if output.endswith("\n") else "\n")
    if rc != 0:
        print(f"west-patch-apply: `west patch apply` failed (rc={rc})", file=sys.stderr)
        return rc

    applied = parse_applied_count(output)
    if applied is None:
        print(
            f"west-patch-apply: `west patch apply` exited 0 but applied no "
            f"patches -- it never printed 'N patches applied successfully'. "
            f"{patches_yml} carries {carried}. This is the silent no-op of "
            f"issue #1392: west treats an unresolvable patches.yml as "
            f"'nothing to do', not an error.",
            file=sys.stderr,
        )
        return 1
    if applied != expected:
        print(
            f"west-patch-apply: applied {applied} of the {expected} patches "
            f"{patches_yml} carries. `west patch apply` skips a patch whose "
            f"`module:` does not resolve to a Zephyr module name and still "
            f"exits 0 (issue #1076, again in #1392) -- check every `module:` "
            f"against the module's own zephyr/module.yml `name:`.",
            file=sys.stderr,
        )
        return 1

    print(f"OK: west patch apply applied {applied} of {expected} patches.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
