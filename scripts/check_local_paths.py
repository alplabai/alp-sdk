#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: no hard-coded home-directory paths in executable / build files.

Checked-in scripts, firmware, examples, CMake, and config files must not
embed an absolute maintainer path like ``/home/<user>/...`` (issue #516).
Such paths make the SDK fail on any other checkout, CI job, or customer
workstation, can consume artifacts from the wrong tree, and leak the
maintainer's local layout into customer-facing files.  Use required env
vars, CLI flags, or paths derived from the script/repo root instead, and
placeholders such as ``<alp-sdk>`` / ``<zephyr-workspace>`` / ``<ti-sdk>``
in comments.

Markdown docs are intentionally NOT scanned here -- ``check_cross_platform.py``
already flags forward-slash home paths in docs (per ADR 0012).

Suppression: add a path (relative to the repo root) to ``ALLOWLIST`` with a
one-line reason.  Reserve it for files that legitimately document the
pattern (e.g. the cross-platform linter and its tests) or archival notes.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# A path that identifies SOMEONE'S MACHINE.
#
# START HERE IF YOU ARE ADDING A SHAPE. Nearly every leaked path in this repo
# exists in TWO SPELLINGS, because the repo is developed from Windows checkouts
# driven through WSL: the same directory appears as `C:\Users\<name>\...` and as
# `/mnt/c/Users/<name>/...`, routinely on adjacent lines of the same command.
# So a sweep that fixes one spelling leaves the other behind, and a gate that
# matches one shape reports clean over the other. When this gate grew from three
# shapes to five, the newly-visible half was LARGER than what the gate had been
# catching -- 61 further leaks against 44. Assume any new shape has a twin.
#
# Seven shapes, each added after it was found leaking past the previous set:
#
#   /home/<user>/                 POSIX home
#   <drive>:\Users\<user>         Windows user profile (either separator)
#   <drive>:\GitHub\              a checkout root that exists on exactly one box
#   /mnt/<drive>/Users/<user>     the SAME Windows profile seen from WSL
#   /mnt/<drive>/GitHub/          the same checkout root seen from WSL
#
# The third shape is not a home directory at all, which is precisely why it
# was invisible: `E:\GitHub\tan-cli\.worktrees\python-executor` names a
# machine as exactly as `/home/<user>/` does.
#
# The last two are the same leak wearing a different hat and were the larger
# half of it: this repo is developed from Windows checkouts driven through WSL,
# so the same directory appears BOTH as `C:\Users\<name>\...` and as
# `/mnt/c/Users/<name>/...`, usually on adjacent lines of the same command. A
# sweep that fixed only the Windows-shaped spelling left 63 occurrences of the
# WSL-mount spelling behind -- more than the 44 the gate was catching at the
# time -- because neither `/home/` nor `<drive>:` matches `/mnt/c/Users/`.
#
# Shapes six and seven are the same machine again, in the two remaining
# spellings this repo actually produces: the UNC form Explorer shows for a WSL
# home (`\\wsl$\Ubuntu-22.04\home\<user>\...`), and the flattened per-project
# directory name a tool derives from an absolute path by replacing separators
# with dashes (`C--Users-<user>-Documents-...`). Neither contains a `/home/`,
# a `<drive>:` or a `/mnt/` and so both walked past the first five branches.
MACHINE_PATH_RE = re.compile(
    r"/home/[A-Za-z0-9._-]+/"
    r"|[A-Za-z]:[\\/]Users[\\/][A-Za-z0-9._ -]+"
    r"|[A-Za-z]:[\\/]GitHub[\\/]"
    r"|/mnt/[A-Za-z]/Users/[A-Za-z0-9._-]+"
    r"|/mnt/[A-Za-z]/GitHub/"
    r"|\\\\wsl\$[\\/][^\\/]+[\\/]home[\\/][A-Za-z0-9._-]+"
    r"|[A-Za-z]--Users-[A-Za-z0-9._-]+"
)

# Matched text that is a DOCUMENTED PLACEHOLDER, not a real machine. Docs and
# the bootstrap scripts have to be able to show the SHAPE of a Windows path;
# flagging those would teach people to allowlist their way past this check,
# which ends worse than the leak it prevents.
# NOTE the absence of a plural `users` alternative, which is not an oversight:
# it matched the literal `\Users\` segment of every Windows profile path and
# silently disabled that whole branch of MACHINE_PATH_RE. The placeholder is
# the USER-NAME segment, never the `Users` directory that precedes it.
PLACEHOLDER_RE = re.compile(
    r"[\\/](?:<[^>]+>|\.\.\.|user|username|%USERNAME%|\$USER|John Smith"
    r"|Jane Doe|dev|you|YourName)(?:[\\/]|$)",
    re.IGNORECASE,
)

# Extensions / names that are executable or build inputs (not prose docs).
SCANNED_SUFFIXES = {
    ".sh", ".ps1", ".bat", ".cmake", ".conf", ".cfg", ".ini",
    ".c", ".h", ".cpp", ".hpp", ".py", ".yaml", ".yml", ".overlay", ".dts", ".dtsi",
    ".kconfig",
    # `.md` and `.json` are scanned too. The docstring above used to say
    # Markdown was deliberately left to `check_cross_platform.py` -- that
    # delegation did not hold: that linter excludes `docs/superpowers/specs`
    # outright, and it matches forward-slash home paths only, so a
    # backslashed `E:\GitHub\...` in a doc was checked by nobody. Prose leaks
    # a machine layout exactly as effectively as a script does.
    ".md", ".json",
}
SCANNED_NAMES = {"CMakeLists.txt", "Makefile", "Kconfig"}

# path (repo-relative) -> reason. Files that legitimately contain a home path.
ALLOWLIST: dict[str, str] = {
    "scripts/check_cross_platform.py": "documents the /home/user pattern it lints",
    "scripts/check_local_paths.py": "this linter documents the pattern in its docstring",
    "tests/scripts/test_check_cross_platform.py": "test fixtures exercise the pattern",
    "tests/scripts/test_check_emit_snapshots.py": "test fixtures feed /home paths to the SDK-root normalizer under test",
    "tests/scripts/test_orchestrate_buildplan.py": "fixture asserts a Windows venv interpreter path survives plan emission verbatim",
}

# Directory prefixes that are archival and exempt.
#
# `docs/superpowers/plans/` USED to be listed here, on the reasoning that
# captured planning history is archival. That exemption is removed: archival
# or not, those files are published in a public repository, and six of them
# were shipping a developer's `E:\GitHub\...` and `C:\Users\<name>\...`
# layout. "Nobody edits it any more" is not a reason for it to be readable by
# everybody. Nothing is exempt now; genuinely-illustrative paths are handled
# by `PLACEHOLDER_RE`, per-file exceptions by `ALLOWLIST` with a reason.
EXEMPT_PREFIXES: tuple[str, ...] = ()


def _tracked_files(root: Path) -> list[str]:
    out = subprocess.run(
        ["git", "-C", str(root), "ls-files"],
        capture_output=True, text=True, check=True,
    )
    return out.stdout.splitlines()


def _is_scanned(rel: str) -> bool:
    p = Path(rel)
    return p.suffix in SCANNED_SUFFIXES or p.name in SCANNED_NAMES


def check(root: Path) -> list[str]:
    offenders: list[str] = []
    for rel in _tracked_files(root):
        if rel in ALLOWLIST:
            continue
        if any(rel.startswith(pfx) for pfx in EXEMPT_PREFIXES):
            continue
        if not _is_scanned(rel):
            continue
        text = (root / rel).read_text(encoding="utf-8", errors="replace")
        for i, line in enumerate(text.splitlines(), start=1):
            for match in MACHINE_PATH_RE.finditer(line):
                if PLACEHOLDER_RE.search(match.group(0)):
                    continue
                offenders.append(
                    f"{rel}:{i}: hard-coded machine path "
                    f"({match.group(0).strip()}): {line.strip()[:120]}")
                break
    return offenders


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    offenders = check(args.root)
    if not offenders:
        return 0

    print("check_local_paths: hard-coded home-directory paths found "
          "(use env vars / flags / script-relative paths or placeholders):")
    for o in offenders:
        print(f"  {o}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
