#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""No tracked file may live under the Doxygen OUTPUT_DIRECTORY (#1573).

The `doxygen-out/` HTML tree was, for a long stretch, a checked-in Doxygen
*build artifact*: nothing regenerated it on merge (`pr-doxygen.yml` only
uploads the built HTML as a workflow artifact, never commits it back), so it
silently drifted from the source docs it was rendered from -- and carried the
drift indefinitely, since no gate compared the two. #1524 found the snapshot
still repeating a retired "never runs `sudo`" claim that #1524 itself had
already removed from `docs/cli.md` / `docs/getting-started.md`.

The fix was to untrack the generated tree entirely (`.gitignore`'d, built
locally via `doxygen docs/doxygen/Doxyfile`, distributed via the existing
`pr-doxygen.yml` "Upload HTML artefact" step). This gate is what stops that
regressing the moment someone runs `doxygen` and `git add -A` again: it reads
`OUTPUT_DIRECTORY` out of `docs/doxygen/Doxyfile` -- rather than hardcoding
the string `doxygen-out` -- so the check keeps working even if the output
directory is ever renamed, which is exactly the drift class that produced
this issue.
"""
from __future__ import annotations

import re
import subprocess
import sys
from pathlib import Path

_HERE = Path(__file__).resolve()
ROOT = _HERE.parent.parent
DOXYFILE = ROOT / "docs" / "doxygen" / "Doxyfile"


def _output_directory(doxyfile: Path) -> str:
    """Parse `OUTPUT_DIRECTORY = <path>` out of a Doxyfile."""
    text = doxyfile.read_text(encoding="utf-8")
    m = re.search(r"(?m)^\s*OUTPUT_DIRECTORY\s*=\s*(\S+)\s*$", text)
    if not m:
        raise ValueError(f"{doxyfile}: no OUTPUT_DIRECTORY line found")
    return m.group(1)


def _tracked_files_under(root: Path, subdir: str) -> list[str]:
    try:
        out = subprocess.run(
            ["git", "ls-files", "--", subdir],
            cwd=root,
            capture_output=True,
            text=True,
            check=True,
        )
    except FileNotFoundError:
        raise RuntimeError(
            "git is required to run check_no_committed_doxygen_output.py "
            "(it walks `git ls-files`, not the working tree, so it only "
            "flags files actually committed)"
        ) from None
    except subprocess.CalledProcessError as e:
        raise RuntimeError(f"git ls-files failed: {e.stderr}") from None
    return [line for line in out.stdout.splitlines() if line]


def find_problems(root: Path) -> list[str]:
    outdir = _output_directory(root / "docs" / "doxygen" / "Doxyfile")
    tracked = _tracked_files_under(root, outdir)
    if not tracked:
        return []
    return [
        f"{len(tracked)} tracked file(s) under {outdir}/, Doxygen's "
        f"OUTPUT_DIRECTORY (docs/doxygen/Doxyfile) -- this is a generated "
        f"build artifact that nothing regenerates on merge and it will "
        f"silently drift from the source docs (#1573). Remove it from git "
        f"(`git rm -r {outdir}`) and rely on {outdir}/ in .gitignore plus "
        f"`pr-doxygen.yml`'s HTML-artefact upload for distribution; first "
        f"offender: {tracked[0]}"
    ]


def main() -> int:
    try:
        problems = find_problems(ROOT)
    except (ValueError, RuntimeError) as e:
        print(f"no-committed-doxygen-output: {e}", file=sys.stderr)
        return 1
    if problems:
        for p in problems:
            print(f"no-committed-doxygen-output: {p}", file=sys.stderr)
        return 1
    print("OK: no tracked files under the Doxygen OUTPUT_DIRECTORY.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
