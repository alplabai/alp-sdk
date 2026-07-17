#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""`west alp-lock` -- write/verify alp.lock (epic #610 WS6-a).

    west alp-lock            # write ./alp.lock for the workspace
    west alp-lock --check    # recompute + diff; nonzero on drift
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

_HERE = Path(__file__).resolve()
sys.path.insert(0, str(_HERE.parent.parent))  # scripts/ -> import alp_lock
import alp_lock  # noqa: E402

try:
    from west.commands import WestCommand  # type: ignore
except ImportError:  # standalone / CI without west
    class WestCommand:  # type: ignore[no-redef]
        def __init__(self, *a, **k): ...


def _workspace_root(explicit: str | None) -> Path:
    if explicit:
        return Path(explicit).resolve()
    # Default: the alp-sdk repo root (scripts/west_commands/ -> repo). alp-sdk
    # is its own west manifest repo, so its root IS the workspace topdir here.
    # If alp-sdk were ever a sub-project of a larger workspace, pass --workspace
    # (or resolve the real `west topdir`) instead.
    return _HERE.parent.parent.parent


def _dump(lock: dict) -> str:
    return json.dumps(lock, indent=2, sort_keys=True) + "\n"


def run(args) -> int:
    root = _workspace_root(args.workspace)
    board = Path(args.board).resolve() if args.board else None
    lock_path = root / "alp.lock"
    if args.check:
        if not lock_path.is_file():
            print(f"alp-lock: {lock_path} missing -- run `west alp-lock` first",
                  file=sys.stderr)
            return 1
        committed = json.loads(lock_path.read_text(encoding="utf-8"))
        drifts = alp_lock.verify_lock(committed, root, board)
        if drifts:
            for d in drifts:
                print(f"alp-lock: {d.path}: locked {d.locked!r} != actual {d.actual!r}",
                      file=sys.stderr)
            # Nothing regenerates alp.lock on a metadata/schema change, so the
            # drift is almost always "someone edited metadata/ and never
            # re-locked".  Name the fix here: without it the message states a
            # digest mismatch and leaves the reader to find the writer.
            print("alp-lock: regenerate with `west alp-lock` "
                  "(or `python3 scripts/west_commands/alp_lock.py --workspace .`) "
                  "and commit the result.", file=sys.stderr)
            return 1
        print(f"alp-lock: {lock_path} matches the workspace.")
        return 0
    lock = alp_lock.build_lock(root, board)
    lock_path.write_text(_dump(lock), encoding="utf-8")
    print(f"alp-lock: wrote {lock_path}")
    return 0


def _add_args(parser) -> None:
    parser.add_argument("--check", action="store_true",
                        help="verify alp.lock against the workspace (nonzero on drift)")
    parser.add_argument("--workspace", help="workspace root (default: west topdir)")
    parser.add_argument("--board", help="board.yaml to record resolution.board from")


class AlpLock(WestCommand):
    def __init__(self) -> None:
        super().__init__("alp-lock",
                         "Write or verify the project's alp.lock dependency lock",
                         "\n".join(__doc__.splitlines()[2:]) if __doc__ else "")

    def do_add_parser(self, parser_adder):  # type: ignore[no-untyped-def]
        parser = parser_adder.add_parser(self.name, help=self.help,
                                         description=self.description)
        _add_args(parser)
        return parser

    def do_run(self, args, _unknown):  # type: ignore[no-untyped-def]
        return run(args)


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="write/verify alp.lock")
    _add_args(ap)
    return run(ap.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
