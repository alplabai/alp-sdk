#!/usr/bin/env python3
# Copyright 2026 Alp Lab AB
# SPDX-License-Identifier: Apache-2.0
"""Refresh metadata/issue-state-snapshot.json from the live issue tracker.

`check_issue_citations.py` (#1950) is offline by design -- see its
docstring. This script is the other half: it re-harvests the same
`#NNNN` driver-status citations that gate looks at (via
`check_issue_citations.find_citations`, so the two never see a different
set of numbers) and records each cited number's current `gh issue view
--json state` in a small committed JSON snapshot.

MANUAL, on purpose (#1950 round 2): there is no scheduled workflow running
this. A prior design ran it weekly and opened a PR via `gh pr create`
authenticated with `${{ github.token }}` -- GitHub does not start
`pull_request` workflows for events raised by `GITHUB_TOKEN`, so `dev`'s
required status checks never reported and that PR could never merge. Run
this by hand -- `python3 scripts/refresh_issue_state_snapshot.py`, then
commit `metadata/issue-state-snapshot.json` if it changed -- whenever
`check_issue_citations.py` warns the snapshot is stale, or after closing an
issue a driver-status block cites. Not a `scripts/check_*.py` gate -- it
writes, it does not check -- so it is exempt from
`check_quality_registry.py`'s four-site rule.

Idempotent: `generated_at` is only re-stamped when the `issues` object it
records actually changed, so a no-op refresh leaves the file byte-identical
and a plain `git diff` tells you whether anything really moved.

Requires the `gh` CLI, authenticated (`GH_TOKEN`/`GITHUB_TOKEN` in the
environment).

Usage:
    python3 scripts/refresh_issue_state_snapshot.py [--out PATH]
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / "scripts"))

from check_issue_citations import SNAPSHOT_RELPATH, find_citations  # noqa: E402


def _issue_state(number: int) -> str | None:
    """`gh issue view <N> --json state -q .state` -- GitHub issues and PRs
    share one number space and `gh issue view` resolves either, so a
    citation that actually names a merged PR (e.g. #730/#764) still
    resolves. Returns None (not guessed) if `gh` can't resolve it at all."""
    try:
        out = subprocess.run(
            ["gh", "issue", "view", str(number), "--json", "state", "-q", ".state"],
            capture_output=True, text=True, check=True, cwd=REPO,
        )
    except FileNotFoundError:
        sys.exit("refresh_issue_state_snapshot: `gh` CLI not found on PATH")
    except subprocess.CalledProcessError:
        return None
    state = out.stdout.strip()
    return state or None


def _pick_generated_at(out_path: Path, issues: dict[str, str]) -> str:
    """The `generated_at` timestamp to write: the existing one, unchanged,
    if `out_path` already records the exact same `issues` object, else a
    fresh UTC stamp. Pulled out of `main()` so the idempotency rule (#1950
    round 2) is unit-testable without a live `gh` call."""
    if out_path.is_file():
        try:
            existing = json.loads(out_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError):
            existing = None
        if isinstance(existing, dict) and existing.get("issues") == issues:
            ts = existing.get("generated_at")
            if isinstance(ts, str):
                return ts
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", type=Path, default=REPO / SNAPSHOT_RELPATH)
    args = ap.parse_args()

    numbers = sorted({c.issue for c in find_citations(REPO)})
    issues: dict[str, str] = {}
    unresolved: list[int] = []
    for n in numbers:
        state = _issue_state(n)
        if state is None:
            unresolved.append(n)
            continue
        issues[str(n)] = state.upper()

    if unresolved:
        print(
            f"refresh_issue_state_snapshot: could not resolve "
            f"{len(unresolved)} cited number(s) via `gh`: {unresolved} -- "
            f"left out of the snapshot; check_issue_citations.py warns "
            f"rather than blocks on a number the snapshot has no record of.",
            file=sys.stderr,
        )

    doc = {"generated_at": _pick_generated_at(args.out, issues), "issues": issues}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(
        json.dumps(doc, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="",
    )
    print(
        f"refresh_issue_state_snapshot: wrote {len(issues)} issue state(s) "
        f"({len(numbers)} cited, {len(unresolved)} unresolved) to {args.out}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
