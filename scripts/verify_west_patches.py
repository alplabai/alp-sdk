#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""Assert every `zephyr/patches.yml` patch is really applied in a workspace (#1392).

`west patch apply` has three paths that do nothing and exit 0. All three are in
`zephyr/scripts/west_commands/patch.py` at Zephyr v4.4.1:

  * `load_yml()` -- `if not os.path.isfile(args.patch_yml)` prints
    `no patches to apply: <path> not found` and returns `None`; `do_run()` then
    returns. `--patch-base`/`--patch-yml` resolve against the MANIFEST REPO, not
    the cwd, so a caller who passes them relative to where they are standing
    lands on a path that does not exist and gets a clean exit.
  * `apply()` -- `patches = yml.get("patches", []); if not patches: return`.
  * `apply()`, per patch -- `mod = self.get_module_path(...); if mod is None:
    continue`. An unresolvable `module:` is skipped with no message at all.

So a clean `west patch apply` is not evidence that anything was applied, and
every layer under it also succeeds: `bootstrap.sh` succeeds, the build succeeds,
the flash succeeds. The first symptom is a board that does not boot the
application, which reads as a hardware fault and costs bench time.

`zephyr/patches/mcuboot/0001-flash_map_extended-alif-mram-base.patch` is the
load-bearing one: without it `flash_device_base()` has no
`alif_mram_flash_controller` handling, so MCUboot computes the chainload vector
table as `0x0 + 0x10000 + 0x800` -- an address in unused ITCM -- and cannot
chainload the application from MRAM.

HOW IT CHECKS
-------------
Per patch, `git apply --reverse --check` in the module's own checkout. That
succeeds iff the patch content is already present, so there is no per-patch
marker symbol to keep in sync -- the patch file IS the specification. `--check`
writes nothing; this never mutates a workspace.

A patch that neither reverse-applies NOR forward-applies is reported
separately, as `DRIFTED` rather than `ABSENT`: the module has been changed on
top of, or instead of, the patch. Both refuse; the distinction is for whoever
has to fix it.

EXIT CODES
----------
  0  every declared patch verified applied
  1  at least one is ABSENT, DRIFTED, or names a module west cannot resolve --
     the #1392 bug, in a workspace that CAN carry the patch
  2  nothing could be inspected (no workspace, unreadable/empty patches.yml)
  3  everything inspectable is applied, but at least one module is not checked
     out in this workspace. `pr-twister-aen.yml` treats this as a failure --
     it compiles those modules; `scripts/bootstrap.sh` warns, because it also
     serves narrower workspaces.

NO SILENT SKIP
--------------
This is deliberately NOT a `scripts/check_*.py` repo gate. Those run in
`pr-metadata-validate.yml` on a bare clone with no west workspace, so the only
thing such a gate could do is skip -- and a gate that always skips is the exact
shape of the bug it would be pretending to close. It runs where a workspace
exists instead: `scripts/bootstrap.sh` and `pr-twister-aen.yml`. A missing
workspace, a missing module checkout, or a `module:` that resolves to nothing
are all ERRORS here, not skips.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent

#: Outcome of a single patch. `APPLIED` is the only one that is not a failure.
APPLIED = "APPLIED"
ABSENT = "ABSENT"
DRIFTED = "DRIFTED"


def west_project_dirs(topdir: Path) -> dict[str, Path]:
    """Every name a `patches.yml` `module:` may legitimately use -> its checkout.

    `west patch` resolves `module:` through `zephyr_module`, which keys on the
    module's OWN `zephyr/module.yml` `name:` -- not on the west project name.
    The two differ in this workspace: `zephyr/patches.yml` says `module: alif`
    while the west project is `hal_alif`. Both are accepted here, plus the
    directory basename (what `zephyr_module` falls back to when `module.yml`
    declares no `name:`, which is how `mcuboot` resolves).
    """
    out = subprocess.run(
        ["west", "list", "-f", "{name}|{abspath}"],
        cwd=topdir,
        capture_output=True,
        text=True,
    )
    if out.returncode != 0:
        raise RuntimeError(f"`west list` failed in {topdir}:\n{out.stderr.strip()}")

    dirs: dict[str, Path] = {}
    for line in out.stdout.splitlines():
        if "|" not in line:
            continue
        name, abspath = line.split("|", 1)
        path = Path(abspath.strip())
        dirs[name.strip()] = path
        dirs.setdefault(path.name, path)
        module_yml = path / "zephyr" / "module.yml"
        if module_yml.is_file():
            try:
                declared = (yaml.safe_load(module_yml.read_text(encoding="utf-8")) or {}).get("name")
            except yaml.YAMLError:
                declared = None
            if declared:
                dirs[str(declared)] = path
    return dirs


def classify(patch_file: Path, module_dir: Path) -> str:
    """APPLIED / ABSENT / DRIFTED for one patch against one checkout."""
    reverse = subprocess.run(
        ["git", "apply", "--reverse", "--check", str(patch_file)],
        cwd=module_dir,
        capture_output=True,
        text=True,
    )
    if reverse.returncode == 0:
        return APPLIED
    forward = subprocess.run(
        ["git", "apply", "--check", str(patch_file)],
        cwd=module_dir,
        capture_output=True,
        text=True,
    )
    return ABSENT if forward.returncode == 0 else DRIFTED


def verify(repo: Path, topdir: Path) -> tuple[list[str], list[str], list[str]]:
    """`(failures, applied, absent_modules)` -- lists of human-readable lines.

    `absent_modules` is kept apart from `failures` because it is a different
    fact: the module is not in this workspace at all, so there is nothing to
    have patched. That is normal for the narrower workspaces some CI jobs
    build, and wrong for `pr-twister-aen.yml`, which compiles those modules.
    The caller decides -- see `main()`'s exit codes.

    Raises `RuntimeError` for a workspace this cannot inspect at all; that is a
    refusal, never a pass.
    """
    patches_yml = repo / "zephyr" / "patches.yml"
    patch_root = repo / "zephyr" / "patches"
    if not patches_yml.is_file():
        raise RuntimeError(f"{patches_yml} does not exist")

    doc = yaml.safe_load(patches_yml.read_text(encoding="utf-8")) or {}
    patches = doc.get("patches") or []
    if not patches:
        raise RuntimeError(
            f"{patches_yml} declares no patches -- nothing to verify, which is "
            f"indistinguishable from a pass. Delete this call site instead."
        )

    dirs = west_project_dirs(topdir)
    failures: list[str] = []
    applied: list[str] = []
    absent_modules: list[str] = []

    for entry in patches:
        rel = entry.get("path")
        module = entry.get("module")
        if not rel or not module:
            raise RuntimeError(f"patches.yml entry has no path/module: {entry!r}")

        patch_file = patch_root / rel
        if not patch_file.is_file():
            raise RuntimeError(f"{patch_file}, declared in patches.yml, does not exist")

        module_dir = dirs.get(module)
        if module_dir is None:
            failures.append(
                f"  UNRESOLVED  {rel}\n"
                f"              module {module!r} matches no west project in {topdir}. "
                f"`west patch apply` skips this silently (patch.py: "
                f"`if mod is None: continue`), which is why it must fail here."
            )
            continue
        if not (module_dir / ".git").exists():
            absent_modules.append(
                f"  NO-CHECKOUT {rel}\n"
                f"              module {module!r} -> {module_dir}, which is not a git "
                f"checkout. Nothing to have patched; run `west update` if this "
                f"workspace is meant to carry it."
            )
            continue

        verdict = classify(patch_file, module_dir)
        if verdict == APPLIED:
            applied.append(f"  APPLIED     {rel}")
        elif verdict == ABSENT:
            failures.append(
                f"  ABSENT      {rel}\n"
                f"              {module_dir} -- the patch applies cleanly, so its "
                f"content is simply not there."
            )
        else:
            failures.append(
                f"  DRIFTED     {rel}\n"
                f"              {module_dir} -- applies neither forward nor in "
                f"reverse. The module has been changed on top of, or instead of, "
                f"this patch."
            )
    return failures, applied, absent_modules


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument(
        "--repo",
        type=Path,
        default=REPO,
        help="alp-sdk checkout carrying zephyr/patches.yml (default: this script's repo)",
    )
    ap.add_argument(
        "--topdir",
        type=Path,
        default=None,
        help="west workspace root (default: asked of `west topdir`)",
    )
    args = ap.parse_args(argv)

    topdir = args.topdir
    if topdir is None:
        probe = subprocess.run(
            ["west", "topdir"], cwd=args.repo, capture_output=True, text=True
        )
        if probe.returncode != 0 or not probe.stdout.strip():
            print(
                f"verify-west-patches: no west workspace resolvable from {args.repo}.\n"
                f"This is a refusal, not a skip: the whole point of #1392 is that a "
                f"run which\ninspects nothing must not report success. Run it from a "
                f"workspace, or pass --topdir.",
                file=sys.stderr,
            )
            return 2
        topdir = Path(probe.stdout.strip())

    try:
        failures, applied, absent_modules = verify(args.repo, topdir)
    except RuntimeError as err:
        print(f"verify-west-patches: {err}", file=sys.stderr)
        return 2

    for line in applied:
        print(line)
    for line in absent_modules:
        print(line)
    # Ordered output in a CI log: stdout and stderr are separately buffered, so
    # without this the APPLIED lines land after the refusal they precede.
    sys.stdout.flush()
    if failures:
        print(
            f"\nverify-west-patches: {len(failures)} of "
            f"{len(failures) + len(applied)} patch(es) declared in "
            f"zephyr/patches.yml are not applied in {topdir}:",
            file=sys.stderr,
        )
        for line in failures:
            print(line, file=sys.stderr)
        print(
            "\n`west patch apply` exits 0 when it applies nothing (#1392), so a clean\n"
            "run of it is not evidence. Re-run it from the alp-sdk checkout with NO\n"
            "--patch-base/--patch-yml flags: those resolve against the manifest repo,\n"
            "not your cwd.",
            file=sys.stderr,
        )
        return 1

    if absent_modules:
        print(
            f"verify-west-patches: {len(applied)} patch(es) verified applied in "
            f"{topdir}; {len(absent_modules)} could not be checked because their "
            f"module is not in this workspace (exit 3).",
            file=sys.stderr,
        )
        return 3

    print(f"verify-west-patches: OK ({len(applied)} patch(es) verified applied in {topdir}).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
