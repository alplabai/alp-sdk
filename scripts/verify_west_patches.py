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
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

import yaml

REPO = Path(__file__).resolve().parent.parent

#: Outcome of a single patch. `APPLIED` is the only one that is not a failure.
APPLIED = "APPLIED"
ABSENT = "ABSENT"
DRIFTED = "DRIFTED"


def west_project_dirs(topdir: Path, west: str = "west") -> dict[str, Path]:
    """Every name a `patches.yml` `module:` may legitimately use -> its checkout.

    `west patch` resolves `module:` through `zephyr_module`, which keys on the
    module's OWN `zephyr/module.yml` `name:` -- not on the west project name.
    The two differ in this workspace: `zephyr/patches.yml` says `module: alif`
    while the west project is `hal_alif`. Both are accepted here, plus the
    directory basename (what `zephyr_module` falls back to when `module.yml`
    declares no `name:`, which is how `mcuboot` resolves).

    `west` defaults to the bare name on PATH, which is NOT where
    `scripts/bootstrap.sh` puts it: bootstrap installs west into the workspace
    venv and invokes it by absolute path. A bare `west` raised
    `FileNotFoundError: [Errno 2] No such file or directory: 'west'` out of
    every CI job that runs bootstrap, and bootstrap then reported it as
    "zephyr/patches.yml is not applied" -- a wrong message for a verifier that
    could not run. Callers pass `--west "${WEST}"`.
    """
    try:
        out = subprocess.run(
            [west, "list", "-f", "{name}|{abspath}"],
            cwd=topdir,
            capture_output=True,
            text=True,
        )
    except OSError as err:
        raise RuntimeError(
            f"cannot execute {west!r}: {err}. bootstrap installs west into the "
            f"workspace venv rather than onto PATH -- pass --west with its path."
        ) from err
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


def _patched_paths(patch_file: Path) -> list[str]:
    """The repo-relative paths a patch writes, from its `+++ b/<path>` lines."""
    paths: list[str] = []
    for line in patch_file.read_text(encoding="utf-8", errors="replace").splitlines():
        if line.startswith("+++ b/"):
            paths.append(line[len("+++ b/"):].strip())
    return paths


def classify_module(patch_files: list[Path], module_dir: Path) -> dict[Path, str]:
    """`{patch -> APPLIED/ABSENT/DRIFTED}` for one module's whole patch stack.

    Per-patch `git apply --reverse --check` is UNSOUND when two patches touch
    the same file, and two of this repo's do:
    `hal_alif/0001-se-service-add-boot-cpu.patch` and
    `hal_alif/0002-se-service-add-public-send-request.patch` both write
    `se_services/zephyr/include/se_service.h` and
    `se_services/zephyr/src/se_service.c`. With 0002 applied on top, reversing
    0001 alone fails on changed context and reports a patch that IS applied as
    DRIFTED. Measured on a tree where both had just applied forward, rc=0 each:

        git apply -R --check 0001.patch            -> rc=1   (false DRIFTED)
        git apply -R --check 0002.patch 0001.patch -> rc=1   (also unsound:
                                                     --check does not stage
                                                     intermediate results)

    So the stack is reversed SEQUENTIALLY, with real writes, into a throwaway
    copy of just the files the patches touch -- never the workspace. Every
    reverse succeeding proves every patch is present. Verified against a
    pristine `hal_alif` at `v2.3.0`: reverse 0002 then 0001 both rc=0 on the
    patched tree, and rc=1 on the unpatched one.
    """
    ordered = list(patch_files)
    with tempfile.TemporaryDirectory(prefix="verify-west-patches-") as tmp:
        scratch = Path(tmp)
        for patch in ordered:
            for rel in _patched_paths(patch):
                src = module_dir / rel
                if not src.is_file():
                    # A file the patch writes is missing entirely -- the stack
                    # cannot be applied, so nothing below could reverse either.
                    return {pf: ABSENT for pf in ordered}
                dst = scratch / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                if not dst.exists():
                    shutil.copy2(src, dst)

        verdicts: dict[Path, str] = {}
        for i in range(len(ordered) - 1, -1, -1):
            res = subprocess.run(
                ["git", "apply", "--reverse", str(ordered[i])],
                cwd=scratch, capture_output=True, text=True,
            )
            if res.returncode == 0:
                verdicts[ordered[i]] = APPLIED
                continue
            # This one is not present. Keep unwinding the rest of the stack
            # rather than condemning it: an absent patch does not make the
            # patches below it absent, and marking them DRIFTED by default
            # turned one genuinely-missing `zephyr/0003` into three failures
            # against a workspace whose other two zephyr patches were applied.
            forward = subprocess.run(
                ["git", "apply", "--check", str(ordered[i])],
                cwd=scratch, capture_output=True, text=True,
            )
            verdicts[ordered[i]] = ABSENT if forward.returncode == 0 else DRIFTED
        return verdicts


def verify(repo: Path, topdir: Path, west: str = "west") -> tuple[list[str], list[str], list[str]]:
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

    dirs = west_project_dirs(topdir, west)
    failures: list[str] = []
    applied: list[str] = []
    absent_modules: list[str] = []
    unapplied_modules: list[str] = []

    # Grouped by module, in declaration order: a module's patches are a STACK,
    # and one is only meaningfully checkable against the others (see
    # `classify_module`).
    by_module: dict[str, list[dict]] = {}
    for entry in patches:
        rel = entry.get("path")
        module = entry.get("module")
        if not rel or not module:
            raise RuntimeError(f"patches.yml entry has no path/module: {entry!r}")
        patch_file = patch_root / rel
        if not patch_file.is_file():
            raise RuntimeError(f"{patch_file}, declared in patches.yml, does not exist")
        by_module.setdefault(str(module), []).append({"rel": rel, "file": patch_file})

    for module, items in by_module.items():
        unapplied_modules_seen = False
        module_dir = dirs.get(module)
        if module_dir is None:
            unapplied_modules.append(module)
            for it in items:
                failures.append(
                    f"  UNRESOLVED  {it['rel']}\n"
                    f"              module {module!r} matches no west project in {topdir}. "
                    f"`west patch apply` skips this silently (patch.py: "
                    f"`if mod is None: continue`), which is why it must fail here."
                )
            continue
        if not (module_dir / ".git").exists():
            for it in items:
                absent_modules.append(
                    f"  NO-CHECKOUT {it['rel']}\n"
                    f"              module {module!r} -> {module_dir}, which is not a git "
                    f"checkout. Nothing to have patched; run `west update` if this "
                    f"workspace is meant to carry it."
                )
            continue

        verdicts = classify_module([it["file"] for it in items], module_dir)
        for it in items:
            verdict = verdicts.get(it["file"], DRIFTED)
            if verdict == APPLIED:
                applied.append(f"  APPLIED     {it['rel']}")
            elif verdict == ABSENT:
                failures.append(
                    f"  ABSENT      {it['rel']}\n"
                    f"              {module_dir} -- the patch applies cleanly, so its "
                    f"content is simply not there."
                )
            else:
                failures.append(
                    f"  DRIFTED     {it['rel']}\n"
                    f"              {module_dir} -- applies neither forward nor in "
                    f"reverse. The module has been changed on top of, or instead of, "
                    f"this patch."
                )
            if verdict != APPLIED:
                unapplied_modules_seen = True
        if unapplied_modules_seen:
            unapplied_modules.append(module)
    return failures, applied, absent_modules, unapplied_modules


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
    ap.add_argument(
        "--list-unapplied",
        action="store_true",
        help="print the `module:` names that still need `west patch apply`, one "
             "per line, and exit 0. A workspace can be PARTIALLY patched -- "
             "pr-getting-started-aen801.yml caches `zephyr` and `modules` but "
             "not `bootloader/mcuboot`, so zephyr arrives patched and mcuboot "
             "arrives fresh -- and `west patch apply` with no --dst-module then "
             "re-applies the ones already there and fails (#1392).",
    )
    ap.add_argument(
        "--west",
        default="west",
        help="the west executable (default: `west` on PATH; bootstrap passes "
             "the workspace venv's copy, which is not on PATH)",
    )
    args = ap.parse_args(argv)

    topdir = args.topdir
    if topdir is None:
        try:
            probe = subprocess.run(
                [args.west, "topdir"], cwd=args.repo, capture_output=True, text=True
            )
        except OSError as err:
            print(f"verify-west-patches: cannot execute {args.west!r}: {err}",
                  file=sys.stderr)
            return 2
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
        failures, applied, absent_modules, unapplied = verify(args.repo, topdir, args.west)
    except RuntimeError as err:
        print(f"verify-west-patches: {err}", file=sys.stderr)
        return 2

    if args.list_unapplied:
        for module in unapplied:
            print(module)
        return 0

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
