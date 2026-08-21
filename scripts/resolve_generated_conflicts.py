#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""Resolve merge conflicts in GENERATED files by regenerating them.

Some files in this tree are derived from other files and committed anyway:
`alp.lock` carries a digest over `metadata/**`, `metadata/catalog.json` is
emitted from the gate registry, the ABI snapshot is emitted from the public
headers, and the `--emit` goldens are emitted from the projects.  Every branch
that touches their inputs rewrites them, so **any two such branches conflict on
them by construction** -- textually, while the two sides do not actually
disagree about anything.  Each side simply computed the value over a different
tree, and neither value is right for the merge; only a value computed over the
MERGED tree is.

Measured 2026-08-12 across the open PRs: `alp.lock` conflicted on 3,
`docs/abi/v<minor>-snapshot.json` on 2, the `--emit` goldens on 2 and
`metadata/catalog.json` on 1 -- and they re-fire on every subsequent merge, so
the cost is paid per PR per merge, not once.

That `alp.lock` count is now historical.  It was inflated by `sdk.revision`,
which recorded the repo's own HEAD and so changed on EVERY commit, putting
`alp.lock` in the conflict set for any pair of branches whatsoever.  #1615
retired the field, so `alp.lock` now conflicts only when two branches genuinely
both touch its real inputs (`metadata/**`, `west.yml`,
`metadata/libraries/*.yaml`, `scripts/requirements.txt`).  It stays in
REGENERATORS below: that is exactly the case where recomputing over the merged
tree is the right answer.  The other three files are unaffected by #1615.

This is the same shape `changelog.d/` (#1395) removed for `CHANGELOG.md`, and
the same remedy: stop hand-merging a file whose content is not in dispute.

Why regenerate rather than a git merge driver
---------------------------------------------
A `merge=<driver>` in `.gitattributes` is tempting, but the driver is invoked
per-file with no guarantee about which OTHER paths git has already written to
the worktree.  `alp.lock`'s digest covers all of `metadata/**`, so a driver that
regenerated it mid-merge could hash a half-materialised tree and produce a
confidently wrong value -- worse than the conflict, because it would be silent.

By the time a conflicted merge STOPS, git has written every cleanly-merged path
to the worktree and left only the conflicted ones marked.  Running afterwards is
therefore the point at which the merged tree is complete, so that is when this
runs.  A deliberate trade: one explicit command instead of an automatic hook
that could be subtly wrong.

Usage
-----
    git merge origin/dev            # stops with conflicts
    # ... resolve any NON-generated conflicts by hand ...
    python3 scripts/resolve_generated_conflicts.py
    git commit

Exit codes:
    0  every conflicted path was generated and has been resolved
    1  non-generated conflicts remain (listed); nothing was regenerated
    2  a regenerator failed, or the script was run outside a merge

Note on partial side effects: regenerators run in sequence, so if a later one
fails (exit 2) the earlier ones have already rewritten their files in the
worktree.  Nothing is `git add`ed in that case, so the index still shows the
conflicts and re-running after fixing the cause is safe.
"""

from __future__ import annotations

import argparse
import fnmatch
import subprocess
import sys
from pathlib import Path

def _repo_root() -> Path:
    """The git worktree we are resolving in -- asked of git, not inferred.

    Deriving this from `__file__` breaks the moment the script is invoked from
    a copy (I hit exactly that: a copy under /tmp made REPO `/`, and the first
    git call died with exit 129 instead of saying anything useful). The
    checkout being merged is the one git resolves from the CWD.
    """
    try:
        out = subprocess.run(
            ["git", "rev-parse", "--show-toplevel"],
            capture_output=True, text=True, check=True,
        ).stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        raise SystemExit(
            "error: not inside a git worktree -- run this from the checkout "
            "whose merge you are resolving."
        )
    return Path(out)


REPO = _repo_root()


def _abi_snapshot_cmd() -> list[str]:
    """The ABI snapshot's filename carries the current minor, so ask for it
    rather than hardcoding a version that goes stale at the next bump."""
    out = subprocess.run(
        [sys.executable, "scripts/abi_snapshot.py", "--print-current-version"],
        cwd=REPO, capture_output=True, text=True, check=True,
    ).stdout.strip()
    return [
        sys.executable, "scripts/abi_snapshot.py",
        "--version", out,
        "--output", f"docs/abi/{out}-snapshot.json",
    ]


# path glob -> argv factory.  A path is matched against every pattern and the
# FIRST match wins.
REGENERATORS: list[tuple[str, object]] = [
    ("alp.lock",
     lambda: [sys.executable, "scripts/west_commands/alp_lock.py"]),
    ("metadata/catalog.json",
     lambda: [sys.executable, "scripts/gen_catalog.py"]),
    ("docs/abi/*-snapshot.json",
     _abi_snapshot_cmd),
    ("tests/fixtures/emit-snapshots/*.snap",
     lambda: [sys.executable, "scripts/check_emit_snapshots.py", "--update"]),
]


def conflicted_paths() -> list[str]:
    """Unmerged paths, via git's own index rather than by grepping for markers.

    Grepping for `<<<<<<<` would be wrong here: CHANGELOG.md legitimately
    CONTAINS indented conflict markers inside an entry that documents them.
    """
    out = subprocess.run(
        ["git", "diff", "--name-only", "--diff-filter=U"],
        cwd=REPO, capture_output=True, text=True, check=True,
    ).stdout
    return [line for line in out.splitlines() if line.strip()]


def regenerator_for(path: str):
    for pattern, factory in REGENERATORS:
        if fnmatch.fnmatch(path, pattern):
            return pattern, factory
    return None, None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--dry-run", action="store_true",
                    help="report what would be regenerated; change nothing")
    args = ap.parse_args()

    paths = conflicted_paths()
    if not paths:
        print("no conflicted paths -- nothing to resolve "
              "(are you mid-merge?)", file=sys.stderr)
        return 2

    generated: dict[str, object] = {}
    other: list[str] = []
    for p in paths:
        _pattern, factory = regenerator_for(p)
        if factory is None:
            other.append(p)
        else:
            generated[p] = factory

    if not generated:
        print("no GENERATED files among the conflicts; nothing this script "
              "can resolve.")
    for p in sorted(generated):
        print(f"  generated: {p}")
    for p in sorted(other):
        print(f"  NOT generated (left alone): {p}")

    if args.dry_run:
        return 1 if other else 0

    # REFUSE while a non-generated conflict remains.  A derived file is
    # computed FROM the tree -- alp.lock digests all of `metadata/**`, the ABI
    # snapshot reads the public headers -- so regenerating while any input is
    # still conflicted would hash a file that literally contains `<<<<<<<`
    # markers and emit a confidently wrong value.  That is worse than the
    # conflict, because the wrong value looks resolved and the gate that would
    # have caught it is the very gate being satisfied.  Order matters: settle
    # the real conflicts first, then regenerate over a clean tree.
    if other:
        print(f"\nrefusing to regenerate: {len(other)} non-generated "
              f"conflict(s) still unresolved.", file=sys.stderr)
        for p in sorted(other):
            print(f"  {p}", file=sys.stderr)
        print("\nA generated file is derived FROM these paths, so regenerating "
              "now would\nhash conflict markers and produce a wrong value that "
              "looks resolved.\nResolve them, then re-run this script.",
              file=sys.stderr)
        return 1

    # Run each distinct regenerator once, even if it owns several paths
    # (--update rewrites every snapshot in one pass).
    ran: set[tuple[str, ...]] = set()
    for p in sorted(generated):
        argv = tuple(generated[p]())
        if argv in ran:
            continue
        ran.add(argv)
        print(f"  $ {' '.join(argv)}")
        res = subprocess.run(list(argv), cwd=REPO)
        if res.returncode != 0:
            print(f"error: regenerator failed for {p} "
                  f"(exit {res.returncode}) -- nothing staged; "
                  f"fix the cause and re-run", file=sys.stderr)
            return 2

    subprocess.run(["git", "add", "--"] + sorted(generated),
                   cwd=REPO, check=True)
    print(f"resolved {len(generated)} generated conflict(s) by regenerating "
          f"over the merged tree.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
